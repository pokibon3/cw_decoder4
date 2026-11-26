//
//	ML デコーダ (DL 方式): ゲートの並び → 文字。mldec.h を参照
//
//	推論は float、重み行列は int8 (行ごとの倍率付き) で持つ。
//	wasm 版と同じソースで動かすため、exp / log は libm を使わずにここで計算する。
//
//	逐次化 (web/tools/ml/stream.py と同じ方式):
//	モデルは文字を「その文字の後のスペース要素」で出す。スペースの長さは
//	次のマークが来るまで決まらないので、そのままでは送信の最後の文字が
//	出ない。そこでスペースが続いている間、経過時間を仮のスペース長として
//	1 歩だけ試し (隠れ状態は確定しない)、出力が文字になったらその場で出す。
//	仮の 1 歩はスペースが短点 KMIN 個分を超えてから試す。符号内スペースの
//	長さで止まったスペースは学習データに無く、出力が当てにならない (F の
//	途中で U になる)。スペースが終わったら確定した長さで正式に 1 歩進め、
//	仮に出した文字は二重に出さない。
//
//	英文・和文のモード:
//	モードは呼び出し側 (decoder.cpp) が持ち、mldec_element / mldec_silence に
//	渡す。出力はそのモードで使える文字の中で確率最大のものを選ぶ (英文で
//	表に無い符号がカナに、その後ろの T が ﾑ に化けるのを防ぐ)。選び方を
//	変えても GRU の隠れ状態には影響しない (出力を入力に戻さないため)。
//	ホレ / ラタを出すと呼び出し側がモードを切り替える。
//	ホレ / ラタの読み落とし (送信の頭のホレ、ノイズを読んだ偽のホレ) に
//	備え、モデルの 1 位が他方のモードの文字 (英文ならカナ、和文なら英字)
//	の文字が SWITCH_RUN 文字続いたら、その文字からモードを切り替える。
//	英文の中でつながった符号が 1 文字だけカナに読まれても切り替わらない。
//
#include <string.h>
#include "mldec.h"
#include "mlweights.h"

#define KMIN_X10 25                 // 仮の 1 歩を試し始めるスペース長 (短点 2.5 個分)
#define PROBE_STEP_MS 10            // 仮の 1 歩を試す間隔
#define PROBE_MAX_MS 3000           // これより長いスペースでは試さない
#define MARK_HIST 12                // 短点の長さの推定に使う直近のマーク数
#define SWITCH_RUN 3                // 他方のモードの文字がこれだけ続いたら切り替える

static void (*emit_fn)(uint8_t ch) = nullptr;
static void (*mode_fn)(uint8_t jp) = nullptr;
static float h_state[ML_LAYERS][ML_H];  // 確定した隠れ状態
static uint8_t started;             // マークを 1 つでも受けたか
static uint8_t prev_label;          // 直前の出力ラベル (CTC の重複をまとめる)
static uint8_t probe_label;         // このスペースで仮に出したラベル (0 = まだ)
static uint32_t probe_next;         // 次に仮の 1 歩を試す経過時間
static uint16_t marks[MARK_HIST];
// 推論の一時領域。DSP のタスク (スタック 8KB) を圧迫しないよう静的に置く。
// ML は DSP のタスクからしか呼ばれないので共有してよい
static float gi[3 * ML_H], gh[3 * ML_H];
static float h_next[ML_LAYERS][ML_H];
static uint8_t mark_n, mark_pos;
static float logit[ML_V];           // 直前の 1 歩の出力 (ラベルごと)
// モードの切り替えの判定 (1 位のラベルの並びで数える)
static uint8_t other_run;           // 他方のモードの文字が続いた数
static uint8_t prev_top;            // 直前の歩の 1 位 (CTC の重複をまとめる)
static uint8_t probe_top;           // このスペースの仮の 1 歩で数えた 1 位 (0 = まだ)
static uint8_t last_jp;             // 前回呼ばれたときのモード (ボタンでの切替を知る)

//==================================================================
//	数学関数 (libm を使わない)
//==================================================================
static float ml_expf(float x)
{
	if (x > 80.0f) x = 80.0f;
	if (x < -80.0f) x = -80.0f;
	// x = k ln2 + r (|r| <= ln2/2)、exp(r) は 6 次までの級数
	float kf = x * 1.44269504f;
	int k = (int)(kf + (kf >= 0 ? 0.5f : -0.5f));
	float r = x - (float)k * 0.69314718f;
	float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666667f +
	          r * (0.0083333333f + r * 0.0013888889f)))));
	union { float f; uint32_t u; } b;
	b.u = (uint32_t)(k + 127) << 23;
	return p * b.f;
}

static float ml_logf(float x)
{
	// x = m 2^e (1 <= m < 2)、ln m = 2 atanh((m-1)/(m+1)) の級数
	union { float f; uint32_t u; } b;
	b.f = x;
	int e = (int)((b.u >> 23) & 0xFF) - 127;
	b.u = (b.u & 0x007FFFFF) | 0x3F800000;
	float t = (b.f - 1.0f) / (b.f + 1.0f), t2 = t * t;
	float s = t * (2.0f + t2 * (0.66666667f + t2 * (0.4f + t2 * (0.28571429f + t2 * 0.22222222f))));
	return s + (float)e * 0.69314718f;
}

static inline float sigm(float x) { return 1.0f / (1.0f + ml_expf(-x)); }
static inline float tanh_(float x) { return 2.0f * sigm(2.0f * x) - 1.0f; }

//==================================================================
//	GRU の 1 歩
//==================================================================
// 1 層ぶん: in (n_in 次元) と h から新しい h を out へ。ih が int8 なら ih_q/ih_s、
// float なら ih_f を使う
static void gru_layer(const float *in, int n_in, const float *ih_f, const int8_t *ih_q,
                      const float *ih_s, const int8_t *hh_q, const float *hh_s,
                      const float *bih, const float *bhh, const float *h, float *out)
{
	for (int r = 0; r < 3 * ML_H; r++) {
		float a = 0.0f;
		if (ih_f) {
			const float *w = ih_f + r * n_in;
			for (int c = 0; c < n_in; c++) a += w[c] * in[c];
		} else {
			const int8_t *w = ih_q + r * n_in;
			for (int c = 0; c < n_in; c++) a += (float)w[c] * in[c];
			a *= ih_s[r];
		}
		gi[r] = a + bih[r];
		const int8_t *u = hh_q + r * ML_H;
		float g = 0.0f;
		for (int c = 0; c < ML_H; c++) g += (float)u[c] * h[c];
		gh[r] = g * hh_s[r] + bhh[r];
	}
	for (int j = 0; j < ML_H; j++) {
		float rg = sigm(gi[j] + gh[j]);
		float zg = sigm(gi[ML_H + j] + gh[ML_H + j]);
		float ng = tanh_(gi[2 * ML_H + j] + rg * gh[2 * ML_H + j]);
		out[j] = (1.0f - zg) * ng + zg * h[j];
	}
}

// そのモードで選べるラベルか。英文: カナとラタ以外。和文: 和文の符号表
// (decode.cpp の jp_code) にある文字 = カナ、数字、? , ! @ : -、ラタ。
// 空白 (語間) と CTC の空白 (0) はどちらでも選べる
static bool label_ok(uint8_t l, uint8_t jp)
{
	if (l == 0) return true;
	uint8_t c = ML_VOCAB[l - 1];
	if (c == ' ') return true;
	if (!jp) return c < 0x80 && c != 6;
	if (c >= 0xA1 || c == 6) return true;
	if (c >= '0' && c <= '9') return true;
	return c == '?' || c == ',' || c == '!' || c == '@' || c == ':' || c == '-';
}

// 他方のモードにしか無い文字か (英文のときのカナ、和文のときの英字とホレ)
static bool is_other(uint8_t l, uint8_t jp)
{
	uint8_t c = ML_VOCAB[l - 1];
	if (!jp) return c >= 0xA1;
	return (c >= 'A' && c <= 'Z') || c == 5;
}

// h から 1 歩進めて hn へ。出力を logit[] に置き、1 位のラベルを返す
static uint8_t step(const float h[ML_LAYERS][ML_H], uint8_t mark, uint32_t ms,
                    float hn[ML_LAYERS][ML_H])
{
	float ms_f = (float)ms;
	if (ms_f < 1.0f) ms_f = 1.0f;
	if (ms_f > 4000.0f) ms_f = 4000.0f;
	float x[2] = { mark ? 1.0f : 0.0f, ml_logf(ms_f / 60.0f) };
	gru_layer(x, 2, ml_ih0, nullptr, nullptr, ml_hh0_q, ml_hh0_s, ml_bih0, ml_bhh0,
	          h[0], hn[0]);
#if ML_LAYERS >= 2
	gru_layer(hn[0], ML_H, nullptr, ml_ih1_q, ml_ih1_s, ml_hh1_q, ml_hh1_s, ml_bih1,
	          ml_bhh1, h[1], hn[1]);
#endif
	const float *top = hn[ML_LAYERS - 1];
	uint8_t best = 0;
	float bv = -1e30f;
	for (int v = 0; v < ML_V; v++) {
		const int8_t *w = ml_out_q + v * ML_H;
		float a = 0.0f;
		for (int c = 0; c < ML_H; c++) a += (float)w[c] * top[c];
		a = a * ml_out_s[v] + ml_out_b[v];
		logit[v] = a;
		if (a > bv) { bv = a; best = (uint8_t)v; }
	}
	return best;
}

// 直前の 1 歩の出力から、モード jp で選べるラベルのうち確率最大のものを返す
static uint8_t pick(uint8_t jp)
{
	uint8_t best = 0;
	float bv = -1e30f;
	for (int v = 0; v < ML_V; v++) {
		if (!label_ok((uint8_t)v, jp)) continue;
		if (logit[v] > bv) { bv = logit[v]; best = (uint8_t)v; }
	}
	return best;
}

// 1 位のラベル top を数え、他方のモードの文字が SWITCH_RUN 文字続いたら
// モードを切り替える。切り替え後のモードを返す。数字・共通の記号・同じ
// モードの文字で数え直し、語間の空白では数え直さない
static uint8_t count_top(uint8_t top, uint8_t jp)
{
	if (top == 0 || top == prev_top) return jp;
	if (is_other(top, jp)) {
		if (++other_run >= SWITCH_RUN) {
			other_run = 0;
			jp = !jp;
			last_jp = jp;
			if (mode_fn) mode_fn(jp);
		}
	} else if (ML_VOCAB[top - 1] != ' ') {
		other_run = 0;
	}
	return jp;
}

//==================================================================
//	出力
//==================================================================
// ラベルを文字にして出す。= + ( は従来のデコーダと同じく BT / AR / KN で出す
// (同じ符号なので区別できない)
static void out_label(uint8_t l)
{
	if (!emit_fn || l == 0) return;
	uint8_t c = ML_VOCAB[l - 1];
	if (c == 5 || c == 6) other_run = 0;    // ホレ / ラタ (呼び出し側が切り替える)
	if (c == '=') { emit_fn('B'); emit_fn('T'); }
	else if (c == '+') { emit_fn('A'); emit_fn('R'); }
	else if (c == '(') { emit_fn('K'); emit_fn('N'); }
	else emit_fn(c);
}

// 直近のマークの短い方 1/4 の長さ (短点の長さの推定)
static uint32_t dot_ms(void)
{
	if (mark_n == 0) return 60;
	uint16_t s[MARK_HIST];
	memcpy(s, marks, sizeof(uint16_t) * mark_n);
	for (int i = 1; i < mark_n; i++) {      // 挿入ソート (最大 12 個)
		uint16_t v = s[i];
		int j = i - 1;
		while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
		s[j + 1] = v;
	}
	return s[mark_n / 4];
}

//==================================================================
//	公開 API
//==================================================================
void mldec_reset(void)
{
	memset(h_state, 0, sizeof(h_state));
	started = 0;
	prev_label = 0;
	probe_label = 0;
	probe_next = 0;
	mark_n = mark_pos = 0;
	other_run = 0;
	prev_top = 0;
	probe_top = 0;
}

void mldec_set_emit(void (*emit)(uint8_t ch))
{
	emit_fn = emit;
}

void mldec_set_mode_cb(void (*fn)(uint8_t jp))
{
	mode_fn = fn;
}

// ボタンでモードが変わっていたら数え直す
static void check_mode(uint8_t jp)
{
	if (jp != last_jp) { last_jp = jp; other_run = 0; }
}

void mldec_element(uint8_t mark, uint32_t ms, uint8_t jp)
{
	if (!mark && !started) return;  // 最初のマークより前の無音は渡さない (学習と同じ)
	check_mode(jp);
	uint8_t top = step(h_state, mark, ms, h_next);
	memcpy(h_state, h_next, sizeof(h_state));
	// 仮の 1 歩で数えたスペースは数えない (同じ文字を二重に数えない)
	if (!probe_top) jp = count_top(top, jp);
	prev_top = top;
	probe_top = 0;
	uint8_t l = pick(jp);
	if (mark) {
		started = 1;
		marks[mark_pos] = (uint16_t)((ms > 65535) ? 65535 : ms);
		mark_pos = (uint8_t)((mark_pos + 1) % MARK_HIST);
		if (mark_n < MARK_HIST) mark_n++;
		if (l != 0 && l != prev_label) out_label(l);
		prev_label = l;
		probe_label = 0;
		probe_next = 0;
		return;
	}
	if (probe_label) {
		prev_label = probe_label;   // 仮に出した文字はもう出ている
	} else {
		if (l != 0 && l != prev_label) out_label(l);
		prev_label = l;
	}
	probe_label = 0;
	probe_next = 0;
}

void mldec_silence(uint32_t elapsed, uint8_t jp)
{
	if (!started || probe_label || elapsed >= PROBE_MAX_MS) return;
	if (probe_next == 0) {
		uint32_t start = (dot_ms() * KMIN_X10) / 10;
		probe_next = (start > PROBE_STEP_MS) ? start : PROBE_STEP_MS;
	}
	if (elapsed < probe_next) return;
	probe_next = elapsed + PROBE_STEP_MS;
	check_mode(jp);
	uint8_t top = step(h_state, 0, elapsed, h_next);
	if (!probe_top && top != 0 && top != prev_top) {
		jp = count_top(top, jp);
		probe_top = top;
	}
	uint8_t l = pick(jp);
	if (l != 0 && l != prev_label) {
		out_label(l);
		probe_label = l;
	}
}
