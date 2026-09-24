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
#include "dsp.h"
#include "scopelog.h"

// トーン判定: 中心ビンがサイドレベルの何倍あれば正弦波とみなすか。
#define TONE_SIDE_RATIO 3
#define TONE_SIDE_RATIO_OFF_X10 25
// 近サイド(±136Hz = ±1 hopビン)比。純音は近サイド≒0で比が大、帯域制限
// ノイズは近サイドにもエネルギーが乗って比が小さくなる。
//
// ON と OFF で比が非対称なのは意図的:
//   ON は全条件の AND なので、ここを上げるとマークの頭が切られる。
//   キーイングの立ち上がりは過渡的に近サイドへ漏れて比が 3 程度まで落ちる
//   ため、3.5 にしたら短点の頭が 22ms 削れて 30ms (0.53unit) になり
//   「短すぎ」で捨てられた (I が E になる)。ON は 2.0 のままにする。
//   OFF は OR なので、上げても振幅条件で ON が成立している限り影響しない。
//   振幅が中途半端な区間 = キーイングのギャップでだけ効き、そこを解除する。
//   振幅 OFF (0.4*limit) だけではギャップの 1 列でしか成立せず、ブロック
//   単位の揺れでノイズブランカのタイマーがリセットされて解除に失敗し、
//   短点と長点が 1 本に融合していた (Y が O、BY が BTM になる)。
// 実測: ギャップの比は最大 2.98、マーク本体は 3.7 以上、短点の内部は 2.57。
#define TONE_NEAR_RATIO_X10 20
#define TONE_NEAR_RATIO_OFF_X10 32
// 包絡線ジッタ棄却: ジッタが中心の何割を超えたらノイズとみなすか (x10)。
// 純音のマーク中はジッタ数%、帯域制限ノイズは30〜50%。3 = 30%。
#define TONE_JITTER_OFF_X10 3
// 実運用の中心速度帯 20〜35wpm の単位長範囲 (ms、マージン込み)。
#define WPM_CORE_UNIT_MIN 30
#define WPM_CORE_UNIT_MAX 66
#define NOISE_BLANKER_ENABLED 1
// 診断: マーク/スペース長・符号列をシリアル出力
#define DEC_DIAG 0
// デコーダ内部時刻はサンプル数から作る (1ブロック = DSP_HOP サンプル =
// 59/8kHz = 7.375ms。整数 ms では表せないのでサンプル数で積算して ms に換算)
#define DEC_SAMPLES_PER_MS (DSP_SAMPLE_RATE / 1000)

static const uint8_t KEY_LOW = 0;
static const uint8_t KEY_HIGH = 1;

static int32_t magnitudelimit = 140;
static int32_t magnitudelimit_low = 140;
// 振幅しきい値 (magnitudelimit) の追従係数: 立ち上がりは速く (1/6 = τ44ms)、
// 下降は遅く (1/48 = τ0.35s) してピークホールド的に振る舞わせる。
// 対称 1/6 だと文字間ギャップで limit がノイズ床まで崩落して振幅条件が
// 無効化され、帯域制限ノイズの偽符号を比率条件だけで防ぐことになる。
// (旧来は絶対床 140 = 振幅 9.5count が「極小入力のときだけ」ホールドとして
//  働いていたため、適正入力レベルが極端に低くなっていた)
#define LIMIT_ATTACK_DIV 6
#define LIMIT_DECAY_DIV 48
// ノイズ床に連動した相対スケルチ: ゲートOFF中の中心マグニチュードを遅い
// EMA (1/128 = τ0.94s) で追い、limit の床を noise_floor x 8 にする
// (ON 下限 = 0.6 x 床 = ノイズ平均 x 4.8)。ノイズの Goertzel 出力はレイリー
// 分布で大きく揺れ、実測ログではノイズ床 589 に対し瞬時値が 2470 (4.2 倍) まで
// 出ていた。x6 では ON 下限が 3.6 倍 = 2120 でこれを下回り、キーイングの
// ギャップ中にノイズでゲートが再点火して短点と長点が 1 本に融合していた
// (Y が O になる)。x8 なら ON 下限 4.8 倍 = 2827 でノイズのピークを超える。
// 代償: マークがノイズ床の 4.8 倍に届かない信号は拾えなくなる。
// (以前 x8 で不調だったのはノイズ床の整数 EMA がラチェットしていたためで、
//  Q8 化で解消済み)
// 絶対床 140 だけだと入力を極端に絞ったときしかスケルチが効かず、通常
// レベルではノイズ棄却が比率条件だけになって帯域制限ノイズの偽符号が増える。
static int32_t noise_floor = 0;
static volatile uint8_t gate_flags = 0;     // bit0=tone_on bit1=tone_off bit2=realstate
static int32_t noise_acc = 0;       // noise_floor の Q8 蓄積値 (整数 EMA のラチェット防止)
#define NOISE_FLOOR_DIV 128
#define NOISE_SQUELCH_X10 80
static uint16_t realstate = KEY_LOW;
static uint16_t realstatebefore = KEY_LOW;
static uint16_t filteredstate = KEY_LOW;
static uint16_t filteredstatebefore = KEY_LOW;
static uint32_t starttimehigh;
static uint32_t highduration;
static uint32_t hightimesavg = 60;
static uint32_t startttimelow;
static uint32_t lowduration;
// キーイングの重み = 符号内スペース長 / 短点長 を Q8 で追う。
//
// 重みは送り手によって 1:1 とは限らない。実測 (POTA QSO 録音): 短点 51ms /
// 長点 165ms に対し符号内スペースが 83ms あり、短点の 1.63 倍だった
// (機械送出の original.mp3 はきっちり 1.00 倍)。長点/短点比は 3.24、語間も
// 6.8 短点で標準的なので、崩れているのは符号内スペースだけ。
// 短点だけから単位長を出して「1.5 単位以上は文字間」と切ると、この音源の
// 符号内スペース 83ms は 1.5 x 51 = 76ms を超えて全て文字間と判定され、
// 1 文字が要素ごとにばらける (デコード結果が E/T だらけになる)。
// 符号内スペースそのものを測って、文字間との境界をそこから出す。
//
// 長さ (ms) ではなく短点長との比で持つのが要点: 起動直後や速度追従中は
// hightimesavg が動くので、絶対値で覚えておくと古い速度の値が残って
// 境界がずれる (35WPM の合成 CW で初期値 60ms が残り、文字間 103ms を
// 符号内と誤判定して CQ CQ が 1 文字に融合した)。
static uint32_t gap_ratio_q8 = 256;         // 256 = 1.00 倍 (理想 CW)
// 暴走防止の拘束: 文字間ギャップを符号内と取り違えると比が伸び、伸びた分
// だけ次の文字間も飲み込む正帰還になる。重み 0.8〜2.0 倍に限る
// (Farnsworth は文字間だけが伸びて符号内は 1 単位のままなので、
//  この下限側に張り付いて従来どおり動く)
#define GAP_RATIO_MIN_Q8 205                // 0.80
#define GAP_RATIO_MAX_Q8 512                // 2.00
static uint8_t nb_acc = 0;          // ノイズブランカの積分値 (0..nb_max)

static char code[20];
static uint16_t stop_flag = KEY_LOW;
static uint16_t wpm;
static uint32_t last_mark_ms = 0;
static uint32_t last_gap_ms = 0;
// 直前の「短すぎるマーク」(短点として採用されない 0.6 単位未満) の長さと
// 連続数。ノイズの偽マークは窓のなまり+ブランカで 20〜30ms に揃いやすく
// 2 連続では防げないので、±25% 以内が 3 連続 (S/H/5 など本物の短点列)
// のときだけ速度推定に入れる
static uint32_t prev_short_mark_ms = 0;
static uint8_t short_mark_run = 0;
#define SHORT_MARK_CONFIRM 3

static char sw_mode = MODE_US;
static uint8_t lastChar = 0;
static void (*emit_fn)(uint8_t ch) = nullptr;

// 現在組み立て中の符号グループの直前ギャップ(先頭エレメント追加時に記録)。
// 孤立した単発エレメント(ノイズ由来のE/T)を検出するのに使う。
static uint32_t code_pre_gap = 0;
// ノイズだけの区間で散発的に E/T が出るのを抑える。
// 1 文字ごとの特徴では本物と区別できない (実測: マーク強度はノイズ由来
// 13.6dB / 本物 20.4dB で分布が重なり、前後の無音で孤立しているものは
// 17% しかなく、単位長推定も崩れていない)。効くのは文脈で、本物の CW にも
// E/T は 19〜43% 含まれるのに対し、ノイズだけの区間では 69〜71% に達する。
// そこで直近 ONE_WIN 文字のうち 1 要素の文字が ONE_THR 個以上を占める
// あいだだけ、1 要素の文字を捨てる。抑止したものも履歴には数えるので、
// ノイズが止まれば自然に復帰する。
// 実測 (7MHz バンドノイズ 10 分、信号なし): 541 文字 → 52 文字。
// 実信号側は qsb/FS/qrm の常用語・コールサイン、Alice 音源の正解率 99%、
// selftest 8 条件 100% のいずれも変化なし。
static uint32_t one_hist = 0;       // 1=1要素 のビット列 (新しいほど下位)
#define ONE_WIN 12
#define ONE_THR 9
// 孤立判定のしきい値(単位長の倍数)。前が これ以上の無音で単一エレメント
// なら「散発ノイズ」とみなす(語間~7単位より大きめ)。
#define ISOLATED_GAP_UNITS 6

// デコーダ内部時刻 (ms)。millis() ではなくサンプル数由来のブロック
// クロックを使う: I2S DMA のバッファリングでブロック処理がまとめて
// 走ると、millis() ではマーク/スペース長がバッファ長単位に量子化され
// 測定を壊すため (実測で32ms単位になりデコード率が劣化した)。
static uint32_t dec_ms = 0;
static uint64_t dec_samples = 0;

//==================================================================
// gap を 1単位 hightimesavg の相対値で分類
//==================================================================
typedef enum {
	GAP_INTRA = 0,
	GAP_CHAR  = 1,
	GAP_WORD  = 2
} gap_type_t;

//	現在の重みから見た符号内スペース長 (ms)
static uint32_t gap_unit(uint32_t unit)
{
	return (unit * gap_ratio_q8) >> 8;
}

//	符号内/文字間の境界 (ms)。実測した符号内スペース長と、あるべき文字間
//	(3 単位) のちょうど中点に置く。
//	  理想 CW (重み 1.0) なら (1u + 3u)/2 = 2.0u … 1 と 3 の真ん中
//	  POTA 録音 (重み 1.63) なら (83 + 153)/2 = 118ms … 符号内 83 と
//	  文字間 170〜250 の間に十分な余裕をもって入る
//	重みが上限 2.0 まで振れても境界は (2u + 3u)/2 = 2.5u にしかならず、
//	3 単位の文字間を飲み込むことは構造上ない (正帰還に入らない)。
static uint32_t gap_char_threshold(uint32_t unit)
{
	return (gap_unit(unit) + unit * 3) / 2;
}

static gap_type_t classify_gap(uint32_t gap, uint32_t unit)
{
	if (unit == 0) return GAP_INTRA;
	if (gap < gap_char_threshold(unit)) {
		return GAP_INTRA;
	}
	// 文字間/語間はマーク由来の unit で切る。語間は符号内スペースほど
	// 重みの影響を受けない (実測の POTA 録音でも語間は 6.8 短点で標準的)
	if (gap < (unit * 9) / 2) {
		return GAP_CHAR;
	}
	if (gap >= unit * 6) {
		return GAP_WORD;
	}
	return GAP_CHAR;
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

// 直近のマーク長を覚えておき、推定が実際から大きく外れたら引き戻す。
//
// 推定が高すぎると、本物の短点は 0.6 単位未満で「短すぎ」として捨てられ、
// 本物の長点は 0.6〜2 単位に入って「短点」と扱われ unit をさらに押し上げる
// (正帰還)。しかも短点が捨てられるので 1:3 のスナップに使うペアも作れず、
// いったん遅い方へ振れると速い CW に戻れなくなる。
// そこで現在の推定に依存しない再同期を用意する: 直近 8 個のマーク長に
// 1:3 の構造 (最短群と最長群の比が 3 前後) があれば、そこから unit を直接
// 求め、現在値と 35% 以上ずれていたら引き戻す。外れ値 1 個に強くするため
// 最小/最大ではなく 2 番目の値を使う。
static uint32_t pending_snap;       // 下で定義 (大きなスナップの保留値)
#define MARK_HIST_N 8
#define REANCHOR_PCT 35
static uint16_t mark_hist[MARK_HIST_N];
static uint8_t mark_hist_pos = 0;
static uint8_t mark_hist_cnt = 0;

static void mark_hist_push(uint32_t ms)
{
	mark_hist[mark_hist_pos] = (uint16_t)((ms > 65535) ? 65535 : ms);
	mark_hist_pos = (uint8_t)((mark_hist_pos + 1) % MARK_HIST_N);
	if (mark_hist_cnt < MARK_HIST_N) mark_hist_cnt++;
}

static void unit_clamp(void);

static void unit_reanchor(void)
{
	if (mark_hist_cnt < MARK_HIST_N) {
		return;
	}
	uint16_t min1 = 65535, min2 = 65535, max1 = 0, max2 = 0;
	for (uint8_t i = 0; i < MARK_HIST_N; i++) {
		uint16_t v = mark_hist[i];
		if (v < min1) { min2 = min1; min1 = v; } else if (v < min2) { min2 = v; }
		if (v > max1) { max2 = max1; max1 = v; } else if (v > max2) { max2 = v; }
	}
	if (min2 == 0) {
		return;
	}
	// 最短群と最長群が 1:3 になっているときだけ信用する
	if ((uint32_t)max2 * 10 < (uint32_t)min2 * 24 ||
	    (uint32_t)max2 * 10 > (uint32_t)min2 * 36) {
		return;
	}
	uint32_t cu = ((uint32_t)min2 + (uint32_t)max2 / 3) / 2;
	if (cu < 18 || cu > 400) {
		return;                     // 明らかに符号ではない
	}
	uint32_t diff = (cu > hightimesavg) ? (cu - hightimesavg) : (hightimesavg - cu);
	if (diff * 100 <= (uint32_t)hightimesavg * REANCHOR_PCT) {
		return;
	}
	hightimesavg = cu;
	unit_clamp();                   // 上限50WPM/下限4WPM に収める
	wpm = (uint16_t)((1200 + hightimesavg / 2) / hightimesavg);
	if (wpm > 50) wpm = 50;
	mark_hist_cnt = 0;              // 引き戻したら通常の追従に任せる
	pending_snap = 0;
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

// 大きなスナップ (現在の unit から ±35% 超) は 2 連続で整合したときだけ
// 適用する。本物の速度変化なら整合ペアが続けて出るが、ノイズで分断された
// マーク/ギャップの断片が偶然 1:3 に見えるのは単発 (実機ログ: ギャップ30 +
// マーク96 で unit 61→31 に飛んだ)
#define SNAP_BIG_PCT 35

static void unit_apply_snap(uint32_t snap)
{
	uint32_t diff = (snap > hightimesavg) ? (snap - hightimesavg)
	                                      : (hightimesavg - snap);
	if (diff * 100 > hightimesavg * SNAP_BIG_PCT) {
		uint32_t hi = (snap > pending_snap) ? snap : pending_snap;
		uint32_t lo = (snap > pending_snap) ? pending_snap : snap;
		if (pending_snap == 0 || lo * 4 < hi * 3) {
			pending_snap = snap;        // 1 回目 (または前回と不整合): 保留
			return;
		}
		pending_snap = 0;               // 2 連続で整合: 採用
	} else {
		pending_snap = 0;
	}
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
	{
		int len = (int)strlen(code);
		uint8_t isone = (len == 1) ? 1 : 0;
		one_hist = (one_hist << 1) | isone;
		uint32_t bits = one_hist & ((1u << ONE_WIN) - 1u);
		int ones = 0;
		for (uint32_t b = bits; b != 0; b >>= 1) ones += (int)(b & 1u);
		if (isone && ones >= ONE_THR) {
			code[0] = '\0';        // ノイズ主体の区間とみなして捨てる
			return;
		}
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
	one_hist = 0;
	noise_floor = 0;
	noise_acc = 0;
	realstate = realstatebefore = KEY_LOW;
	filteredstate = filteredstatebefore = KEY_LOW;
	hightimesavg = 60;
	gap_ratio_q8 = 256;
	highduration = 0;
	lowduration = 0;
	last_mark_ms = 0;
	last_gap_ms = 0;
	prev_short_mark_ms = 0;
	short_mark_run = 0;
	pending_snap = 0;
	mark_hist_pos = 0;
	mark_hist_cnt = 0;
	wpm = 20;                       // 起動時の表示/窓選択の既定 (hightimesavg=60ms と一致)
	code[0] = '\0';
	lastChar = 0;
	stop_flag = KEY_LOW;
	dec_ms = 0;
	dec_samples = 0;
	nb_acc = 0;
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

int32_t decoder_noise_floor(void)
{
	return noise_floor;
}

uint8_t decoder_gate_flags(void)
{
	return gate_flags;
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
	dec_samples += DSP_HOP;
	dec_ms = (uint32_t)(dec_samples / DEC_SAMPLES_PER_MS);

	// 立ち上がり(現在LOW)時のみ瞬時サイドも見る:
	// 広帯域インパルスはEMAが追従する前の1ブロック目をすり抜けるため。
	if (filteredstate == KEY_LOW && side_mag_inst > side_mag) {
		side_mag = side_mag_inst;
	}

	// ノイズ床の更新。判定状態 (realstate) で選別すると、ON に失敗した信号が
	// 丸ごとノイズ床に取り込まれて床が信号レベルまで上がり二度と ON に
	// ならない (正帰還のデッドロック) ので、ブロックの性質で選別する:
	//   - 振幅がノイズ床の 2 倍未満 (ノイズの揺らぎの範囲) か、
	//   - トーンらしくない (中心 <= サイド x 2 の広帯域) ブロック
	// だけを取り込む。CW のマークは狭帯域なので状態に関係なく除外され、
	// ノイズが本当に増えたときは広帯域なので追従する。
	// EMA は Q8 で蓄積する: 整数 (mag - nf) / 128 だと |差| < 128 で 1 も
	// 動かず、信号エッジで上がる一方の片道ラチェットになる (実機ログで
	// 無音 mag=18 なのに nf=107 のまま、信号中に 326 まで上昇し ON
	// しきい値が信号を超えてマークが削られた)
	{
		uint8_t tone_like = (magnitude > side_mag * 2);
		if (noise_acc == 0) {
			noise_acc = magnitude << 8;
		} else if (!tone_like || magnitude < noise_floor) {
			noise_acc += ((magnitude << 8) - noise_acc) / NOISE_FLOOR_DIV;
		}
		noise_floor = noise_acc >> 8;
	}
	int32_t limit_floor = noise_floor * NOISE_SQUELCH_X10 / 10;
	if (limit_floor < magnitudelimit_low) {
		limit_floor = magnitudelimit_low;
	}

	// 振幅しきい値を自動更新 (立ち上がり速く、下降は遅く)、床はノイズ連動
	if (magnitude > magnitudelimit_low) {
		int32_t d = magnitude - magnitudelimit;
		magnitudelimit += (d > 0) ? d / LIMIT_ATTACK_DIV : d / LIMIT_DECAY_DIV;
	}
	if (magnitudelimit < limit_floor) {
		magnitudelimit = limit_floor;
	}

	// 振幅しきい値 + 中心/サイド比でトーン判定 (ヒステリシス付き)
	// tone_on には「中心が両サイドの max を超える」条件も課す:
	// サイド判定は min(L,H) のため (片側混信保護)、通過帯域より下の
	// 帯域外ノイズが下側サイドだけを上げつつ中心へ漏れると、静かな
	// 上側サイドとの比較をすり抜けて偽符号が出る。本物のトーンは
	// ±100Hz 程度ズレていても中心が両サイドより必ず大きい。
	//
	// さらに帯域制限ノイズ(狭帯域フィルタ後の白色ノイズ)対策:
	//  近サイド(±136Hz): 中心 > 近サイド×比。ノイズは近サイドも上がって
	//  比が小さくなるので棄却され、純音は通過する。キーイングのギャップの
	//  解除もこの条件が担う (振幅条件だけでは足りない。上の定義を参照)
	// (包絡線ジッタ案はキーイングのエッジと区別できず実信号を削るため不採用)
	(void)jitter;
	{
		uint8_t tone_on  = (((uint32_t)magnitude * 5U) > ((uint32_t)magnitudelimit * 3U)) &&
		                   (magnitude > side_mag * TONE_SIDE_RATIO) &&
		                   (magnitude > side_mag_max) &&
		                   (magnitude * 10 > near_side * TONE_NEAR_RATIO_X10);
		uint8_t tone_off = (((uint32_t)magnitude * 5U) < ((uint32_t)magnitudelimit * 2U)) ||
		                   (magnitude * 10 < side_mag * TONE_SIDE_RATIO_OFF_X10) ||
		                   (magnitude * 10 < near_side * TONE_NEAR_RATIO_OFF_X10);
		if (tone_on) {
			realstate = KEY_HIGH;
		} else if (tone_off) {
			realstate = KEY_LOW;
		}
		gate_flags = (uint8_t)((tone_on ? 1 : 0) | (tone_off ? 2 : 0) |
		                       ((realstate == KEY_HIGH) ? 4 : 0));
	}

	// ノイズブランカ: realstate を上下カウンタで積分して filteredstate を出す。
	//
	// 旧「nbtime ms 安定したら反映」方式は、しきい値付近で realstate が
	// ブロック毎に往復すると laststarttime が毎回リセットされ、安定条件が
	// 永久に満たされずに状態が反映されない。実測ログではキーイングの
	// ギャップ中の realstate が 3 ブロック中 1 しか HIGH でないのにゲートは
	// ON のままで、短点 2 つが 1 本の長点に融合していた (S が R になる)。
	// また nbtime は 8ms 上限にクランプされており、20WPM では単位長の
	// 0.13 しかなく、数ブロックのノイズのパルスも通していた。
	//
	// 積分方式なら往復していても多数決で収束し、短いパルスも潰せる。
	// 反転に必要なブロック数は単位長の約 0.35 (短点より十分短い)。
	// 立ち上がりと立ち下がりが同じだけ遅れるので要素長は保たれる。
#if NOISE_BLANKER_ENABLED
	{
		uint32_t unit = (hightimesavg > 0) ? hightimesavg : highduration;
		// 0.35 単位ぶんのブロック数 (四捨五入)。ホップ長から計算するので
		// DSP_HOP を変えても比率は保たれる。切り捨てると 20WPM で 2 ブロックに
		// なり、2 ブロックのドロップアウトでカウンタが空になって符号が切れる
		// (N の長点が途中で切れて T + E になった実測例)
		uint32_t n = ((unit * 35 * (DSP_SAMPLE_RATE / 1000)) + 50 * DSP_HOP) /
		             (100 * DSP_HOP);
		if (n < 2) n = 2;
		// 上限は推定が外れたときの保険。深さは推定値から計算するので、
		// 推定が遅い側へ外れると深さが伸びて、速い CW の短点やギャップを
		// まるごと飲み込んでしまう (10WPM の推定 120ms では深さ 41ms となり、
		// 40WPM の短点 30ms もギャップ 30ms も消えてゲートが動かず、推定を
		// 直すための材料すら測れなくなる)。対応最速 50WPM の短点 24ms を
		// 必ず残せる 5 ブロック (18.75ms) で頭打ちにする
		if (n > 5) n = 5;
		// 立ち上がりと立ち下がりを必ず同じ段数だけ遅らせる。旧方式にあった
		// 「長い無音のあとは即座に ON」の特例をここで使うと、その要素だけ
		// 立ち上がりが遅れず立ち下がりだけ遅れるため n ブロック (20WPM で
		// 22ms = 0.35単位) 長く測られ、文字群の先頭ごとに速度推定が上振れする
		if (realstate == KEY_HIGH) {
			if (nb_acc < (uint8_t)n) nb_acc++;
		} else if (nb_acc > 0) {
			nb_acc--;
		}
		if (nb_acc >= (uint8_t)n) {
			filteredstate = KEY_HIGH;
		} else if (nb_acc == 0) {
			filteredstate = KEY_LOW;
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
			scopelog_element(0, lowduration, hightimesavg);
#if DEC_DIAG
			Serial.printf("[dec] S %4lu u=%lu\n", (unsigned long)lowduration,
			              (unsigned long)hightimesavg);
#endif
			// 重みの追従。符号内と判定できたギャップだけを取り込む。
			// 取り込む値も 0.8〜2.0 に丸めてから平均するので、文字間を
			// 取り違えても比が上限を越えて伸びることはない
			if (lowduration >= 20 && hightimesavg > 0 &&
			    lowduration < gap_char_threshold(hightimesavg)) {
				uint32_t r = (lowduration << 8) / hightimesavg;
				if (r < GAP_RATIO_MIN_Q8) r = GAP_RATIO_MIN_Q8;
				if (r > GAP_RATIO_MAX_Q8) r = GAP_RATIO_MAX_Q8;
				if (r >= gap_ratio_q8) {
					gap_ratio_q8 += (r - gap_ratio_q8) / 3;
				} else {
					gap_ratio_q8 -= (gap_ratio_q8 - r) / 3;
				}
			}
			// ギャップは直前マークとの 1:3 スナップにだけ使う。単独の平滑更新は
			// しない: ノイズの偽マークで分断されたギャップの断片や送信の癖で
			// 1 回に unit が 1/3 も動き、速度が暴れる (マークは短点1/長点3 と
			// 長さが決まっているので信頼できるが、ギャップはそうではない)
			if (lowduration >= 20) {
				if (lowduration < 5 * hightimesavg) {
					uint32_t snap = (last_mark_ms >= 20)
						? snap_candidate(lowduration, last_mark_ms) : 0;
					if (snap != 0) {
						unit_apply_snap(snap);
					}
				}
				last_gap_ms = lowduration;
			}
		}
		if (filteredstate == KEY_LOW) {
			highduration = (dec_ms - starttimehigh);
			scopelog_element(1, highduration, hightimesavg);
			// 符号にならない短いマーク (ノイズの単発) でギャップを分断しない。
			// startttimelow を進めなければ前後の無音が 1 つのギャップとして
			// 測られる。これが無いと文字間ギャップが 2 つに割れて短くなり、
			// 文字が繋がってしまう (TH が 6 になる)
			if (hightimesavg == 0 || (uint32_t)highduration * 5 >= (uint32_t)hightimesavg * 3) {
				startttimelow = dec_ms;
			}
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
			// 短点にも満たない短いマーク (0.6 単位未満) はノイズの疑いが濃い:
			// 単発では速度推定に入れない (unit_smooth_update が 1 回で 1/3 も
			// 引き下げ、2〜3 発で 30WPM 超 → 短窓へ切り替わる連鎖の入口)。
			// 同程度 (±25%) の短いマークが 2 連続したときだけ本物の速度上昇
			// とみなして通す (スナップ範囲外の大きな速度変化もこれで追従)
			uint8_t mark_short = (highduration * 5U < hightimesavg * 3U);
			uint8_t short_consistent = 0;
			if (mark_short) {
				uint8_t same = 0;
				if (prev_short_mark_ms != 0) {
					uint32_t hi = (highduration > prev_short_mark_ms) ? highduration : prev_short_mark_ms;
					uint32_t lo = (highduration > prev_short_mark_ms) ? prev_short_mark_ms : highduration;
					same = (lo * 4 >= hi * 3);
				}
				short_mark_run = same ? (uint8_t)(short_mark_run + 1) : 1;
				short_consistent = (short_mark_run >= SHORT_MARK_CONFIRM);
				prev_short_mark_ms = highduration;
			} else {
				prev_short_mark_ms = 0;
				short_mark_run = 0;
			}
			// 推定に依存しない再同期のため、判定フィルタを通す前に記録する
			if (highduration >= 20 && !mark_isolated) {
				mark_hist_push(highduration);
				unit_reanchor();
			}
			if (highduration >= 20 && !mark_isolated && (!mark_short || short_consistent)) {
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
				prev_short_mark_ms = 0;
				short_mark_run = 0;
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
