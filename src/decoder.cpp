//
//	CWデコーダ状態機械 (ESP32移植版)
//	CH32V006版 cw_decoder.cpp v1.9 のロジックを移植:
//	- トーン判定ヒステリシス (ON 0.6x/3x, OFF 0.4x/2.5x)
//	- ノイズブランカ (短点長の1/5〜1/3, 3〜8ms)
//	- ギャップ(文字内/文字間)も使う速度推定 + 20〜35wpm帯スナップ
//	入力(トーン検出)は Goertzel から FFT ベースに変更 (dsp.cpp 側)。
//
#include <Arduino.h>
#include <string.h>
#include "decoder.h"
#include "decode.h"

// トーン判定: 中心ビンがサイドレベルの何倍あれば正弦波とみなすか。
#define TONE_SIDE_RATIO 3
#define TONE_SIDE_RATIO_OFF_X10 25
// 近サイド(±166.67Hz)比。純音は近サイド≒0で比が大、330Hz以上の
// フィルタノイズは近サイド≒中心で比≒1になり棄却される。
#define TONE_NEAR_RATIO 2
#define TONE_NEAR_RATIO_OFF_X10 15
// 包絡線ジッタ棄却: ジッタが中心の何割を超えたらノイズとみなすか (x10)。
// 純音のマーク中はジッタ数%、帯域制限ノイズは30〜50%。3 = 30%。
#define TONE_JITTER_OFF_X10 3
// 実運用の中心速度帯 20〜35wpm の単位長範囲 (ms、マージン込み)。
#define WPM_CORE_UNIT_MIN 30
#define WPM_CORE_UNIT_MAX 66
#define NOISE_BLANKER_ENABLED 1
// 診断: マーク/スペース長・符号列をシリアル出力
#define DEC_DIAG 0
// デコーダ内部時刻の1ブロック分 (48サンプル @8kHz = 6ms)
#define DEC_BLOCK_MS 6

static const uint8_t KEY_LOW = 0;
static const uint8_t KEY_HIGH = 1;

static int32_t magnitudelimit = 140;
static int32_t magnitudelimit_low = 140;
static uint16_t realstate = KEY_LOW;
static uint16_t realstatebefore = KEY_LOW;
static uint16_t filteredstate = KEY_LOW;
static uint16_t filteredstatebefore = KEY_LOW;
static uint32_t starttimehigh;
static uint32_t highduration;
static uint32_t hightimesavg = 60;
static uint32_t startttimelow;
static uint32_t lowduration;
static uint32_t laststarttime = 0;
static uint16_t nbtime = 6;

static char code[20];
static uint16_t stop_flag = KEY_LOW;
static uint16_t wpm;
static uint32_t last_mark_ms = 0;
static uint32_t last_gap_ms = 0;

static char sw_mode = MODE_US;
static uint8_t lastChar = 0;
static void (*emit_fn)(uint8_t ch) = nullptr;

// 現在組み立て中の符号グループの直前ギャップ(先頭エレメント追加時に記録)。
// 孤立した単発エレメント(ノイズ由来のE/T)を検出するのに使う。
static uint32_t code_pre_gap = 0;
// 孤立判定のしきい値(単位長の倍数)。前が これ以上の無音で単一エレメント
// なら「散発ノイズ」とみなす(語間~7単位より大きめ)。
#define ISOLATED_GAP_UNITS 6

// デコーダ内部時刻 (ms)。millis() ではなくサンプル数由来のブロック
// クロックを使う: I2S DMA のバッファリングでブロック処理がまとめて
// 走ると、millis() ではマーク/スペース長がバッファ長単位に量子化され
// 測定を壊すため (実測で32ms単位になりデコード率が劣化した)。
static uint32_t dec_ms = 0;

//==================================================================
// gap を 1単位 hightimesavg の相対値で分類
//==================================================================
typedef enum {
	GAP_INTRA = 0,
	GAP_CHAR  = 1,
	GAP_WORD  = 2
} gap_type_t;

static gap_type_t classify_gap(uint32_t gap, uint32_t unit)
{
	if (unit == 0) return GAP_INTRA;
	if (gap < (unit * 3) / 2) {
		return GAP_INTRA;
	}
	if (gap < (unit * 9) / 2) {
		return GAP_CHAR;
	}
	if (gap >= unit * 6) {
		return GAP_WORD;
	}
	return GAP_CHAR;
}

//==================================================================
//	ノイズブランカ時間を短点長から算出
//==================================================================
static uint16_t compute_nbtime(uint32_t unit_ms)
{
	if (unit_ms == 0) return 10;
	uint32_t t = unit_ms / 5;
	if (t < (unit_ms / 3)) {
		uint32_t max_t = unit_ms / 3;
		if (t > max_t) t = max_t;
	}
	if (t < 3) t = 3;
	if (t > 8) t = 8;
	return (uint16_t)t;
}

//==================================================================
//	単位長(短点)の推定ヘルパー
//==================================================================
static uint32_t snap_candidate(uint32_t a, uint32_t b)
{
	uint32_t hi = (a > b) ? a : b;
	uint32_t lo = (a > b) ? b : a;
	if ((hi * 10) >= (lo * 24) && (hi * 10) <= (lo * 36)) {
		uint32_t cu = (lo + hi / 3) / 2;
		if (cu >= WPM_CORE_UNIT_MIN && cu <= WPM_CORE_UNIT_MAX) {
			return cu;
		}
	}
	return 0;
}

static void unit_clamp(void)
{
	if (hightimesavg < 24) {
		hightimesavg = 24;
	}
	if (hightimesavg > 300) {
		hightimesavg = 300;
	}
}

static void unit_apply_snap(uint32_t snap)
{
	uint32_t diff = (snap > hightimesavg) ? (snap - hightimesavg)
	                                      : (hightimesavg - snap);
	if (diff * 4 > hightimesavg) {
		hightimesavg = snap;
	} else if (snap >= hightimesavg) {
		hightimesavg += (snap - hightimesavg) / 3;
	} else {
		hightimesavg -= (hightimesavg - snap) / 3;
	}
	unit_clamp();
}

static void unit_smooth_update(uint32_t dur)
{
	uint32_t est = dur;
	if (dur >= (2 * hightimesavg) && hightimesavg != 0) {
		est = dur / 3;
		if (est > hightimesavg * 2) {
			est = hightimesavg * 2;
		}
	}
	if (est >= hightimesavg) {
		hightimesavg += (est - hightimesavg) / 3;
	} else {
		hightimesavg -= (hightimesavg - est) / 3;
	}
	unit_clamp();
}

//==================================================================
//	デコード結果の出力
//==================================================================
static void emit(uint8_t ch)
{
	if (emit_fn) emit_fn(ch);
}

static int decodeAscii(int16_t asciinumber)
{
	if (asciinumber == 0) return 0;
	if (lastChar == 32 && asciinumber == 32) return 0;

	if        (asciinumber == 1) {			// AR
		emit('A'); emit('R');
	} else if (asciinumber == 2) {			// KN
		emit('K'); emit('N');
	} else if (asciinumber == 3) {			// BT
		emit('B'); emit('T');
	} else if (asciinumber == 4) {			// VA
		emit('V'); emit('A');
	} else if (asciinumber == 7) {			// HH (訂正)
		emit('H'); emit('H');
	} else if (asciinumber == 8) {			// BK
		emit('B'); emit('K');
	} else {
		emit((uint8_t)asciinumber);
	}
	lastChar = (uint8_t)asciinumber;
	return 0;
}

static void decode_and_display(void)
{
	if (strlen(code) == 0) return;
	// 散発ノイズ対策: 長い無音の後の単発エレメント(孤立した . or -)は
	// ノイズの兆候。そのたびに速度推定をゆっくり 20WPM(単位60ms)側へ
	// 寄せる。単位が上がるとノイズマークが「短すぎ」判定になりEが減る。
	// 本物の受信では孤立単発は出ないので通常の速度追従には影響しない。
	if (strlen(code) == 1 && hightimesavg > 0 && hightimesavg < 60 &&
	    code_pre_gap >= hightimesavg * ISOLATED_GAP_UNITS) {
		hightimesavg += (60 - hightimesavg) / 8 + 1;
		if (hightimesavg > 60) hightimesavg = 60;
		wpm = (uint16_t)((1200 + hightimesavg / 2) / hightimesavg);
	}
	int16_t result = decode(code, &sw_mode);
#if DEC_DIAG
	Serial.printf("[dec] code=%-8s -> %d '%c'\n", code, result,
	              (result >= 0x20 && result < 0x7F) ? (char)result : '?');
#endif
	if (result == 0) {
		emit('*');
	} else {
		decodeAscii(result);
	}
	code[0] = '\0';
}

//==================================================================
//	公開API
//==================================================================
void decoder_init(void)
{
	magnitudelimit = magnitudelimit_low;
	realstate = realstatebefore = KEY_LOW;
	filteredstate = filteredstatebefore = KEY_LOW;
	hightimesavg = 60;
	highduration = 0;
	lowduration = 0;
	last_mark_ms = 0;
	last_gap_ms = 0;
	wpm = 0;
	code[0] = '\0';
	lastChar = 0;
	stop_flag = KEY_LOW;
	dec_ms = 0;
	laststarttime = 0;
	starttimehigh = 0;
	startttimelow = 0;
	code_pre_gap = 0;
}

void decoder_set_emit(void (*fn)(uint8_t ch))
{
	emit_fn = fn;
}

uint16_t decoder_wpm(void)
{
	return wpm;
}

uint8_t decoder_gate(void)
{
	return (uint8_t)filteredstate;
}

int32_t decoder_maglimit(void)
{
	return magnitudelimit;
}

uint8_t decoder_mode(void)
{
	return (uint8_t)sw_mode;
}

void decoder_toggle_mode(void)
{
	sw_mode ^= 1;
	code[0] = '\0';
}

//==================================================================
//	ブロック処理本体 (CH32版 cwDecoder ループ1周分)
//==================================================================
void decoder_process_block(int32_t magnitude, int32_t side_mag, int32_t side_mag_inst,
                           int32_t side_mag_max, int32_t near_side, int32_t jitter)
{
	dec_ms += DEC_BLOCK_MS;

	// 立ち上がり(現在LOW)時のみ瞬時サイドも見る:
	// 広帯域インパルスはEMAが追従する前の1ブロック目をすり抜けるため。
	if (filteredstate == KEY_LOW && side_mag_inst > side_mag) {
		side_mag = side_mag_inst;
	}

	// 振幅しきい値を自動更新
	if (magnitude > magnitudelimit_low) {
		magnitudelimit = magnitudelimit + ((magnitude - magnitudelimit) / 6);
	}
	if (magnitudelimit < magnitudelimit_low) {
		magnitudelimit = magnitudelimit_low;
	}

	// 振幅しきい値 + 中心/サイド比でトーン判定 (ヒステリシス付き)
	// tone_on には「中心が両サイドの max を超える」条件も課す:
	// サイド判定は min(L,H) のため (片側混信保護)、通過帯域より下の
	// 帯域外ノイズが下側サイドだけを上げつつ中心へ漏れると、静かな
	// 上側サイドとの比較をすり抜けて偽符号が出る。本物のトーンは
	// ±100Hz 程度ズレていても中心が両サイドより必ず大きい。
	//
	// さらに帯域制限ノイズ(狭帯域フィルタ後の白色ノイズ)対策:
	//  近サイド(±166.67Hz): 中心 > 近サイド×比。フィルタ幅≧330Hz の
	//  ノイズは近サイドが上がり比が小さくなって棄却、純音は通過。
	// (包絡線ジッタ案はキーイングのエッジと区別できず実信号を削るため不採用)
	(void)jitter;
	{
		uint8_t tone_on  = (((uint32_t)magnitude * 5U) > ((uint32_t)magnitudelimit * 3U)) &&
		                   (magnitude > side_mag * TONE_SIDE_RATIO) &&
		                   (magnitude > side_mag_max) &&
		                   (magnitude > near_side * TONE_NEAR_RATIO);
		uint8_t tone_off = (((uint32_t)magnitude * 5U) < ((uint32_t)magnitudelimit * 2U)) ||
		                   (magnitude * 10 < side_mag * TONE_SIDE_RATIO_OFF_X10) ||
		                   (magnitude * 10 < near_side * TONE_NEAR_RATIO_OFF_X10);
		if (tone_on) {
			realstate = KEY_HIGH;
		} else if (tone_off) {
			realstate = KEY_LOW;
		}
	}

	// ノイズブランカで状態を安定化
	if (realstate != realstatebefore) {
		laststarttime = dec_ms;
	}
#if NOISE_BLANKER_ENABLED
	{
		uint32_t unit = (hightimesavg > 0) ? hightimesavg : highduration;
		nbtime = compute_nbtime(unit);
		if (filteredstate == KEY_LOW && realstate == KEY_HIGH && lowduration > unit * 6) {
			filteredstate = realstate;
		}
	}
	if ((dec_ms - laststarttime) > nbtime) {
		if (realstate != filteredstate) {
			filteredstate = realstate;
		}
	}
#else
	filteredstate = realstate;
#endif

	// HIGH/LOW の継続時間を計測 + 速度推定
	if (filteredstate != filteredstatebefore) {
		if (filteredstate == KEY_HIGH) {
			starttimehigh = dec_ms;
			lowduration = (dec_ms - startttimelow);
#if DEC_DIAG
			Serial.printf("[dec] S %4lu u=%lu\n", (unsigned long)lowduration,
			              (unsigned long)hightimesavg);
#endif
			if (lowduration >= 20) {
				if (lowduration < 5 * hightimesavg) {
					uint32_t snap = (last_mark_ms >= 20)
						? snap_candidate(lowduration, last_mark_ms) : 0;
					if (snap != 0) {
						unit_apply_snap(snap);
					} else {
						unit_smooth_update(lowduration);
					}
				}
				last_gap_ms = lowduration;
			}
		}
		if (filteredstate == KEY_LOW) {
			startttimelow = dec_ms;
			highduration = (dec_ms - starttimehigh);
#if DEC_DIAG
			Serial.printf("[dec] M %4lu u=%lu\n", (unsigned long)highduration,
			              (unsigned long)hightimesavg);
#endif
			// 長い無音の後の孤立マークは速度推定に使わない: ノイズの単発が
			// unit を自分の長さ(~36ms)へ引きずり下げるのを防ぐ。これが無いと
			// 散発Eの「上げ」と綱引きになり 30WPM 止まりになる。連続キーイング
			// 中(前ギャップが短い)の本物のマークは通常どおり更新。
			uint8_t mark_isolated =
				(hightimesavg > 0 && lowduration >= hightimesavg * ISOLATED_GAP_UNITS);
			if (highduration >= 20 && !mark_isolated) {
				uint32_t snap = 0;
				if (last_mark_ms >= 20) {
					snap = snap_candidate(highduration, last_mark_ms);
				}
				if (snap == 0 && last_gap_ms >= 20) {
					snap = snap_candidate(highduration, last_gap_ms);
				}
				if (snap != 0) {
					unit_apply_snap(snap);
				} else {
					unit_smooth_update(highduration);
				}
				wpm = (uint16_t)((1200 + hightimesavg / 2) / hightimesavg);
				if (wpm > 50) {
					wpm = 50;
				}
				last_mark_ms = highduration;
			} else if (mark_isolated) {
				last_mark_ms = 0;
				last_gap_ms = 0;
			}
		}
	}

	// 短点/長点判定と休止(1/3/7単位)の判定
	if (filteredstate != filteredstatebefore) {
		stop_flag = KEY_LOW;
		if (filteredstate == KEY_LOW) {
			if (highduration < (hightimesavg * 2) && ((uint32_t)highduration * 5U) > ((uint32_t)hightimesavg * 3U)) {
				if (strlen(code) >= 8) { decode_and_display(); }
				if (code[0] == '\0') { code_pre_gap = lowduration; }
				strcat(code, ".");
			}
			if (highduration > (hightimesavg * 2) && highduration < (hightimesavg * 6)) {
				if (strlen(code) >= 8) { decode_and_display(); }
				if (code[0] == '\0') { code_pre_gap = lowduration; }
				strcat(code, "-");
			}
		}
	}
	if (filteredstate == KEY_HIGH) {
		if (hightimesavg > 0) {
			gap_type_t g = classify_gap(lowduration, hightimesavg);
			if (g == GAP_CHAR) {
				decode_and_display();
			} else if (g == GAP_WORD) {
				decode_and_display();
				decodeAscii(32);
			}
		}
	}

	// 一定時間無音なら確定出力
	{
		uint32_t unit = (hightimesavg > 0) ? hightimesavg : highduration;
		if ((dec_ms - startttimelow) > unit * 6 && stop_flag == KEY_LOW) {
			decode_and_display();
			stop_flag = KEY_HIGH;
		}
	}

	realstatebefore = realstate;
	filteredstatebefore = filteredstate;
}
