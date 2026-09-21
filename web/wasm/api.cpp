//
//	WASM 公開 API — ブラウザ (AudioWorklet) と中核コードの間の橋渡し。
//
//	中核 (src/dsp.cpp / decoder.cpp / decode.cpp / lib/float_fft) は
//	ファームウェアと同一のソースをそのままビルドしている。ここが担うのは
//	ESP32 側で audio.cpp / display.cpp / scopelog.cpp がやっていた役割:
//	  - 音声の供給          : ADC DMA の代わりに AudioWorklet が push する
//	  - デコード文字の取り出し: LCD 描画の代わりにイベント列へ積む
//	  - 表示データの取り出し  : スプライト描画の代わりに JS が読む構造体へ詰める
//
//	スレッドは AudioWorklet の 1 本だけなので、ロックは要らない。
//
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "dsp.h"
#include "audio.h"
#include "decoder.h"
#include "decode.h"
#include "scopelog.h"

#define WASM_EXPORT extern "C" __attribute__((visibility("default")))

//==================================================================
//	Arduino スタブの実体
//==================================================================
SerialStub Serial;
EspStub ESP;

static uint64_t g_samples = 0;          // 取り込んだ 8kHz サンプル数

extern "C" unsigned long millis(void)
{
	return (unsigned long)(g_samples * 1000ULL / DSP_SAMPLE_RATE);
}

extern "C" void delay(unsigned long ms)
{
	(void)ms;                           // ブロックできないので何もしない
}

//	decoder.cpp が呼ぶスコープログ。Web 版ではログ機能を持たないので捨てる
//	(scopelog.h の宣言と同じ C++ リンケージで定義すること)
void scopelog_element(uint8_t mark, uint32_t ms, uint32_t unit)
{
	(void)mark; (void)ms; (void)unit;
}

//==================================================================
//	音声入力 (audio.h の実装)
//
//	ESP32 版の audio.cpp は ADC continuous DMA から 12bit 値を読んで
//	4 点平均で 8kHz にしていた。Web 版では AudioWorklet 側で 8kHz まで
//	落としたものを cw_push() で受け取り、ここでは同じ 0..4095 の
//	「ADC カウント」としてリングに積むだけにする。
//	こうすると dsp.cpp の DC 除去とスケール (±2048 カウント) が
//	ファームウェアとまったく同じ数値レンジで動く。
//==================================================================
#define IN_RING_SIZE 8192               // 1 秒ぶん以上 (8kHz)
static uint16_t in_ring[IN_RING_SIZE];
static uint32_t in_w = 0;               // 書き込み位置 (通し番号)
static uint32_t in_r = 0;               // 読み出し位置 (通し番号)
static uint32_t clip_count = 0;

void audio_init(void) { }
void audio_stop(void) { }

uint32_t audio_clip_total(void)
{
	return clip_count;
}

size_t audio_read(uint16_t *dst, size_t n)
{
	// 1 ブロック揃っていなければ 0 を返す。dsp_step() はそのまま抜ける
	if ((uint32_t)(in_w - in_r) < (uint32_t)n) return 0;
	for (size_t i = 0; i < n; i++) {
		dst[i] = in_ring[(in_r + (uint32_t)i) % IN_RING_SIZE];
	}
	in_r += (uint32_t)n;
	return n;
}

//==================================================================
//	デコード文字イベント
//	ESP32 版の display_enqueue() 相当。スコープ上に文字を置くための
//	「符号区間の中央の列番号」も同じ計算で求める。
//==================================================================
#define CHEV_MAX 64
typedef struct {
	uint32_t col;                       // スコープ列番号 (符号区間の中央)
	uint8_t ch;                         // デコーダ出力バイト (JIS X 0201 カナを含む)
	uint8_t pad[3];
} chev_t;
static chev_t chev[CHEV_MAX];
static uint32_t chev_n = 0;

static void on_emit(uint8_t ch)
{
	uint32_t col;
	if (ch != ' ') {
		uint32_t st, en;
		dsp_scope_char_span(&st, &en);
		if (en < st) en = st;
		col = (st + en) / 2;
	} else {
		col = dsp_scope_col_index();
	}
	if (chev_n < CHEV_MAX) {
		chev[chev_n].ch = ch;
		chev[chev_n].col = col;
		chev_n++;
	}
}

//==================================================================
//	JS へ渡す状態ブロック
//	すべて 4 バイト境界の u32/i32 だけで構成してあるので、JS からは
//	Int32Array / Uint32Array 1 本で読める (構造体レイアウトの取り違えが起きない)
//==================================================================
typedef struct {
	uint32_t scope_total;               // 生成済みスコープ列の通し番号
	uint32_t scope_first;               // 今回返した列の先頭番号
	uint32_t scope_lost;                // リング溢れで取れなかった列数
	uint32_t scope_n;                   // 今回返した列数
	uint32_t char_n;                    // 今回返したデコード文字数
	uint32_t wpm;
	uint32_t tone_hz;                   // ゲート中心 (AUTO 時は追従値)
	uint32_t tone_idx;                  // 0..4 = 手動 / 5 = AUTO
	uint32_t tone_auto;
	uint32_t peak_hz;                   // FFT 検出ピーク (無信号で 0)
	uint32_t gate_bw;                   // トーン判定帯域幅 (Hz)
	uint32_t scope_period_q8;
	uint32_t scope_col_ms_x10;          // スコープ 1 列の時間 (0.1ms 単位)
	int32_t  input_peak;                // 入力ピーク (カウント、0..2048)
	uint32_t input_pct;
	uint32_t mode;                      // MODE_US / MODE_JP
	uint32_t gate;                      // 現在のキー判定
	uint32_t clip_total;
} cw_status_t;
static cw_status_t status;

//	スペクトラム (bin 0..64、31.25Hz/bin)
static uint16_t spec_out[DSP_SPEC_BINS + 1];

//	スコープ列。JS が読みやすいよう 1 列 8 バイトの平坦な形に詰め直す:
//	  +0 i16 mn / +2 i16 mx / +4 u16 mag / +6 u8 gate / +7 u8 hops
#define SCOPE_STAGE_MAX 512
#define SCOPE_STAGE_STRIDE 8
static uint8_t scope_out[SCOPE_STAGE_MAX * SCOPE_STAGE_STRIDE];
static scope_col_t scope_tmp[SCOPE_STAGE_MAX];

//	cw_push() が使う受け渡しバッファ (JS がここへ 0..4095 を書く)
#define IN_STAGE_MAX 4096
static uint16_t in_stage[IN_STAGE_MAX];

//==================================================================
//	公開 API
//==================================================================
WASM_EXPORT void cw_init(void)
{
	decoder_init();
	decoder_set_emit(on_emit);
	dsp_start();                        // 係数/Hann 窓の初期化 (タスクは生成されない)
}

WASM_EXPORT uint16_t *cw_in_ptr(void)   { return in_stage; }
WASM_EXPORT int cw_in_cap(void)         { return IN_STAGE_MAX; }
WASM_EXPORT void *cw_status_ptr(void)   { return &status; }
WASM_EXPORT void *cw_spec_ptr(void)     { return spec_out; }
WASM_EXPORT void *cw_scope_ptr(void)    { return scope_out; }
WASM_EXPORT void *cw_chars_ptr(void)    { return chev; }
WASM_EXPORT int cw_scope_stride(void)   { return SCOPE_STAGE_STRIDE; }
WASM_EXPORT int cw_spec_bins(void)      { return DSP_SPEC_BINS + 1; }
WASM_EXPORT int cw_sample_rate(void)    { return DSP_SAMPLE_RATE; }

//	in_stage に置いた n サンプル (0..4095) を取り込む。
//	リングが溢れる場合は古い方を捨てる (表示が一瞬飛ぶだけで、
//	デコーダの時間軸はサンプル数由来なので破綻しない)
WASM_EXPORT void cw_push(int n)
{
	if (n <= 0) return;
	if (n > IN_STAGE_MAX) n = IN_STAGE_MAX;
	for (int i = 0; i < n; i++) {
		uint16_t v = in_stage[i];
		if (v > 4095) v = 4095;
		if (v <= 4 || v >= 4090) clip_count++;   // ハードクリップ検出
		in_ring[in_w % IN_RING_SIZE] = v;
		in_w++;
	}
	g_samples += (uint64_t)n;
	uint32_t avail = in_w - in_r;
	if (avail > IN_RING_SIZE) in_r = in_w - IN_RING_SIZE;
}

//	溜まっているぶんをブロック単位で処理する。戻り値は処理したブロック数
WASM_EXPORT int cw_run(void)
{
	int blocks = 0;
	while ((uint32_t)(in_w - in_r) >= (uint32_t)DSP_HOP) {
		dsp_step();
		blocks++;
	}
	return blocks;
}

//	状態・スペクトラム・列 from_idx 以降のスコープ列・デコード文字をまとめて詰める
WASM_EXPORT void cw_poll(uint32_t from_idx, int max_cols)
{
	if (max_cols > SCOPE_STAGE_MAX) max_cols = SCOPE_STAGE_MAX;
	if (max_cols < 0) max_cols = 0;

	uint32_t first = from_idx, lost = 0;
	int n = dsp_get_scope_since(from_idx, scope_tmp, max_cols, &first, &lost);
	for (int i = 0; i < n; i++) {
		uint8_t *p = &scope_out[i * SCOPE_STAGE_STRIDE];
		int16_t mn = scope_tmp[i].mn, mx = scope_tmp[i].mx;
		uint16_t mg = scope_tmp[i].mag;
		memcpy(p + 0, &mn, 2);
		memcpy(p + 2, &mx, 2);
		memcpy(p + 4, &mg, 2);
		p[6] = scope_tmp[i].gate;
		p[7] = scope_tmp[i].hops;
	}

	dsp_get_spectrum(spec_out);

	status.scope_total      = dsp_scope_col_index();
	status.scope_first      = first;
	status.scope_lost       = lost;
	status.scope_n          = (uint32_t)n;
	status.char_n           = chev_n;
	status.wpm              = decoder_wpm();
	status.tone_hz          = dsp_tone_hz();
	status.tone_idx         = dsp_tone_index();
	status.tone_auto        = dsp_tone_is_auto();
	status.peak_hz          = dsp_peak_hz();
	status.gate_bw          = dsp_gate_bw_hz();
	status.scope_period_q8  = dsp_scope_period_q8();
	status.scope_col_ms_x10 = dsp_scope_col_ms_x10();
	status.input_peak       = dsp_input_peak();
	status.input_pct        = dsp_input_level_pct();
	status.mode             = decoder_mode();
	status.gate             = decoder_gate();
	status.clip_total       = clip_count;

	chev_n = 0;                         // 読み出したので次のフレームへ
}

WASM_EXPORT void cw_set_tone(int idx)
{
	if (idx < 0 || idx > DSP_TONE_AUTO) idx = DSP_TONE_AUTO;
	dsp_set_tone((uint8_t)idx);
}

//	FFT パネルのタップ相当: 指定周波数にいちばん近い手動トーンを選ぶ
WASM_EXPORT void cw_set_tone_hz(int hz)
{
	int best = 0, bestd = 0x7FFFFFFF;
	for (int i = 0; i < DSP_TONE_COUNT; i++) {
		int d = (int)dsp_tone_hz_at((uint8_t)i) - hz;
		if (d < 0) d = -d;
		if (d < bestd) { bestd = d; best = i; }
	}
	dsp_set_tone((uint8_t)best);
}

//	トーンボタン: AUTO → 600 → 700 → 800 → 900 → 1000 → AUTO の順送り
WASM_EXPORT void cw_cycle_tone(void)
{
	uint8_t idx = dsp_tone_index();
	dsp_set_tone((uint8_t)((idx >= DSP_TONE_AUTO) ? 0 : (idx + 1)));
}

WASM_EXPORT void cw_toggle_mode(void)
{
	decoder_toggle_mode();
}

//	入力を切り替えたとき用: デコーダの状態と入力リングを捨てる
WASM_EXPORT void cw_reset(void)
{
	in_r = in_w;
	chev_n = 0;
	clip_count = 0;
	decoder_init();
	decoder_set_emit(on_emit);
}
