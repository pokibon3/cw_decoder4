//
//	DSP タスク (Core 0)
//	- I2S DMA から 48サンプル(6ms)単位で取得
//	- トーン判定は 3ビン float Goertzel (CH32版 v1.8/1.9 と同一方式)。
//	  サイドは EMA(α=1/4) 平滑と瞬時値の両方をデコーダへ渡す。
//	- 256pt FFT (Hann窓) でスペクトラム表示用データ生成
//	- オシロ用に hop 毎の生波形 min/max + エンベロープ + ゲートを記録
//
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "dsp.h"
#include "audio.h"
#include "decoder.h"
#include "float_fft.h"

#define SCOPE_RING_SIZE 256
// スコープ掃引はWPM追従: 1列のhop数 = 60/WPM (Q8の分数蓄積でリニアに)。
// 20WPM=3.0hop(18ms/列)、40WPM=1.5hop(9ms/列)。範囲外はクランプ。
#define SCOPE_HOPS_Q8_MIN 384   // 1.5 hop (40WPM)
#define SCOPE_HOPS_Q8_MAX 768   // 3.0 hop (20WPM以下)
#define SPEC_INTERVAL_HOPS 6    // 36ms 毎 (約28fps)
#define DSP_AUTO_MIN_MAG 800    // AUTO同調とピーク表示の共通絶対床
#define DSP_DIAG 0              // 毎秒 blk/s・mag統計をシリアル出力

#if DSP_DIAG
static uint32_t diag_blocks = 0;
static int32_t diag_mag_max = 0;
static int64_t diag_mag_sum = 0;
static int64_t diag_side_sum = 0;
static int64_t diag_smax_sum = 0;
static int32_t diag_smax_max = 0;
static int64_t diag_near_sum = 0;
static int64_t diag_jit_sum = 0;
#endif

static const uint16_t tone_tbl[DSP_TONE_COUNT] = { 600, 700, 800, 900, 1000 };
static volatile uint8_t tone_sel = DSP_TONE_AUTO;   // デフォルトはAUTO
static volatile uint16_t gate_hz = 700;             // ゲート中心 (AUTO待機時は帯域中央)

// AUTO同調の候補追跡
static uint16_t cand_hz = 0;
static uint8_t cand_cnt = 0;
static uint16_t peak_cand_hz = 0;
static uint8_t peak_cand_cnt = 0;

static int16_t sample_ring[512];
static uint16_t sample_pos = 0;
static int32_t dc_est = 2048 * 256;   // 生ADC値の直流分 (Q8)

static scope_col_t scope_ring[SCOPE_RING_SIZE];
static uint16_t scope_pos = 0;
static volatile uint16_t scope_period_q8 = 768;   // 現在の1列hop数 (Q8)
static volatile uint8_t input_pct = 0;            // 入力レベル (フルスケール比%)

static uint16_t spec_mag[DSP_SPEC_BINS + 1];
static uint16_t peak_hz = 0;
static float hann[DSP_SPEC_N];

static int32_t side_ema_l = 0;
static int32_t side_ema_h = 0;
static uint8_t side_ema_started = 0;

// トーン判定窓長 (サンプル数)。WPM追従で切替:
// 12ms窓(96)はエッジが±6msなまり、45WPM(ギャップ27ms)では要素間
// ギャップが20ms未満に潰れて短点が融合する。高速時は v1.9 実証済みの
// 6ms窓(48)へ戻す。ヒステリシス: ≥32WPMで48 / ≤28WPMで96。
static volatile uint8_t gate_win = DSP_GATE_WIN;

static portMUX_TYPE dsp_mux = portMUX_INITIALIZER_UNLOCKED;

static void gate_update_coeff(void);

void dsp_set_tone(uint8_t idx)
{
	if (idx > DSP_TONE_AUTO) idx = 0;
	tone_sel = idx;
	if (idx != DSP_TONE_AUTO) {
		gate_hz = tone_tbl[idx];
	}
	// AUTO選択時は現在の中心を維持したまま追従を再開する
	side_ema_started = 0;
	cand_cnt = 0;
	gate_update_coeff();
}

uint8_t dsp_tone_index(void)
{
	return tone_sel;
}

uint8_t dsp_tone_is_auto(void)
{
	return (tone_sel == DSP_TONE_AUTO) ? 1 : 0;
}

uint16_t dsp_tone_hz(void)
{
	return gate_hz;
}

uint16_t dsp_tone_hz_at(uint8_t idx)
{
	return tone_tbl[(idx < DSP_TONE_COUNT) ? idx : 0];
}

int dsp_get_scope(scope_col_t *out, int n)
{
	if (n > SCOPE_RING_SIZE) n = SCOPE_RING_SIZE;
	taskENTER_CRITICAL(&dsp_mux);
	uint16_t pos = scope_pos;
	for (int i = 0; i < n; i++) {
		out[i] = scope_ring[(pos + SCOPE_RING_SIZE - n + i) % SCOPE_RING_SIZE];
	}
	taskEXIT_CRITICAL(&dsp_mux);
	return n;
}

void dsp_get_spectrum(uint16_t *out)
{
	taskENTER_CRITICAL(&dsp_mux);
	memcpy(out, spec_mag, sizeof(spec_mag));
	taskEXIT_CRITICAL(&dsp_mux);
}

uint16_t dsp_peak_hz(void)
{
	return peak_hz;
}

uint16_t dsp_scope_col_ms_x10(void)
{
	// 1 hop = 6ms → 0.1ms単位で 60
	return (uint16_t)(((uint32_t)scope_period_q8 * 60U) >> 8);
}

uint16_t dsp_gate_bw_hz(void)
{
	return (uint16_t)(DSP_SAMPLE_RATE / gate_win);
}

uint8_t dsp_input_level_pct(void)
{
	return input_pct;
}

//==================================================================
//	トーン判定: 3ビン float Goertzel (CH32版 v1.8/1.9 と同一方式)
//	窓長は 96 サンプル (12ms)、ホップ 48 (判定周期 6ms は不変)。
//	帯域幅 83.3Hz と CH32版(167Hz)の半分で、帯域内ノイズ電力が
//	半減 = SNR +3dB。重畳バンドノイズ下の弱信号対策。
//	代償: トーン同調が中心±40Hz程度までシビアになる。
//	サイド = ±333.33Hz は96サンプル窓では±4 DFTビンにあたり、
//	引き続き矩形窓 Dirichlet核のヌル上 (純音はサイドへ漏れない)。
//	(FFTゲートはヌル配置が崩れトーンの周波数ズレに弱く、実機で
//	 デコード率が劣化したため廃止。FFT はスペアナ表示専用)
//	戻り値: デコーダ正規化後の中心マグニチュード
//==================================================================
static float g_coeff_c = 0.0f;
static float g_coeff_l = 0.0f;    // 遠サイド -333.33Hz (±2/±4 bin)
static float g_coeff_h = 0.0f;    // 遠サイド +333.33Hz
static float g_coeff_nl = 0.0f;   // 近サイド -166.67Hz (±1/±2 bin)
static float g_coeff_nh = 0.0f;   // 近サイド +166.67Hz

// 近サイド用EMA + 包絡線ジッタ
static int32_t near_ema_l = 0;
static int32_t near_ema_h = 0;
static int32_t jitter_ema = 0;
static int32_t mag_prev = 0;

static void gate_update_coeff(void)
{
	float fc = (float)gate_hz;
	float fstep = (float)DSP_SAMPLE_RATE / (float)DSP_HOP;   // 166.67Hz
	g_coeff_c  = 2.0f * cosf(2.0f * (float)M_PI * fc / (float)DSP_SAMPLE_RATE);
	g_coeff_l  = 2.0f * cosf(2.0f * (float)M_PI * (fc - 2.0f * fstep) / (float)DSP_SAMPLE_RATE);
	g_coeff_h  = 2.0f * cosf(2.0f * (float)M_PI * (fc + 2.0f * fstep) / (float)DSP_SAMPLE_RATE);
	g_coeff_nl = 2.0f * cosf(2.0f * (float)M_PI * (fc - fstep) / (float)DSP_SAMPLE_RATE);
	g_coeff_nh = 2.0f * cosf(2.0f * (float)M_PI * (fc + fstep) / (float)DSP_SAMPLE_RATE);
}

static inline int32_t goertzel_mag(float q1, float q2, float coeff)
{
	float mag2 = (q1 * q1) + (q2 * q2) - (q1 * q2 * coeff);
	if (mag2 < 0.0f) {
		mag2 = 0.0f;
	}
	return (int32_t)(sqrtf(mag2) + 0.5f);
}

static int32_t process_gate(void)
{
	// 直近 gate_win サンプルを窓として使う (96時はホップ48で50%重複)
	const int n = gate_win;
	float win[DSP_GATE_WIN];
	float mean = 0.0f;
	uint16_t base = (uint16_t)((sample_pos + 512 - n) % 512);
	for (int i = 0; i < n; i++) {
		win[i] = (float)sample_ring[(base + i) % 512];
		mean += win[i];
	}
	mean /= (float)n;

	float q1c = 0.0f, q2c = 0.0f;
	float q1l = 0.0f, q2l = 0.0f;
	float q1h = 0.0f, q2h = 0.0f;
	float q1nl = 0.0f, q2nl = 0.0f;
	float q1nh = 0.0f, q2nh = 0.0f;
	for (int i = 0; i < n; i++) {
		const float x = win[i] - mean;
		float q0;
		q0 = g_coeff_c  * q1c  - q2c  + x; q2c  = q1c;  q1c  = q0;
		q0 = g_coeff_l  * q1l  - q2l  + x; q2l  = q1l;  q1l  = q0;
		q0 = g_coeff_h  * q1h  - q2h  + x; q2h  = q1h;  q1h  = q0;
		q0 = g_coeff_nl * q1nl - q2nl + x; q2nl = q1nl; q1nl = q0;
		q0 = g_coeff_nh * q1nh - q2nh + x; q2nh = q1nh; q1nh = q0;
	}

	int32_t mag_c = goertzel_mag(q1c, q2c, g_coeff_c);
	int32_t mag_l = goertzel_mag(q1l, q2l, g_coeff_l);
	int32_t mag_h = goertzel_mag(q1h, q2h, g_coeff_h);
	int32_t mag_nl = goertzel_mag(q1nl, q2nl, g_coeff_nl);
	int32_t mag_nh = goertzel_mag(q1nh, q2nh, g_coeff_nh);

	int32_t side_inst = (mag_l < mag_h) ? mag_l : mag_h;
	int32_t side_inst_max = (mag_l > mag_h) ? mag_l : mag_h;
	if (!side_ema_started) {
		side_ema_started = 1;
		side_ema_l = mag_l;
		side_ema_h = mag_h;
		near_ema_l = mag_nl;
		near_ema_h = mag_nh;
		jitter_ema = 0;
		mag_prev = mag_c;
	} else {
		side_ema_l += (mag_l - side_ema_l) / 4;
		side_ema_h += (mag_h - side_ema_h) / 4;
		near_ema_l += (mag_nl - near_ema_l) / 4;
		near_ema_h += (mag_nh - near_ema_h) / 4;
	}
	// 近サイド(±166.67Hz)。330Hz程度以上のフィルタノイズは通過帯域内の
	// この位置が上がるが、純音(帯域<100Hz)はヌル上で低いまま。
	// = 「トーンより広い帯域か」の判定。min採用で片側の隣接信号に耐性。
	int32_t near_side = (near_ema_l < near_ema_h) ? near_ema_l : near_ema_h;
	// 包絡線ジッタ: 中心マグニチュードのブロック間変化のEMA(α=1/8)。
	// 純音は安定(小)、帯域制限ノイズはブロック毎に揺れる(大)。
	{
		int32_t d = mag_c - mag_prev;
		if (d < 0) d = -d;
		jitter_ema += (d - jitter_ema) >> 3;
		mag_prev = mag_c;
	}
	// 比率判定用サイド: min(L,H) ではなく幾何平均 sqrt(L*H) を使う。
	// 低域から通過帯域へ裾を引く傾斜ノイズでは、min が静かな上側だけを
	// 見て帯域内ノイズの実力を過小評価し偽符号が出る。幾何平均は両サイド
	// の中間周波数の実効ノイズ床に相当し、トーン受信時(両サイド静粛)は
	// min とほぼ同値なので感度は落ちない。
	int32_t side = (int32_t)(sqrtf((float)side_ema_l * (float)side_ema_h) + 0.5f);
	// トーンON拒否用のサイドmax: EMAと現在ブロック瞬時値の大きい方。
	// EMAだけだと広帯域バーストの立ち上がりで中心の瞬時スパイクが
	// 平滑化済みサイドを追い越し、偽マークが漏れる。
	int32_t side_max = (side_ema_l > side_ema_h) ? side_ema_l : side_ema_h;
	if (side_inst_max > side_max) side_max = side_inst_max;

	// 旧 normalize_decoder_magnitude() と同じ >>2
	int32_t mag_norm = mag_c >> 2;
	int32_t side_norm = side >> 2;
	int32_t side_inst_norm = side_inst >> 2;
	int32_t side_max_norm = side_max >> 2;
	int32_t near_norm = near_side >> 2;
	int32_t jitter_norm = jitter_ema >> 2;

	decoder_process_block(mag_norm, side_norm, side_inst_norm, side_max_norm,
	                      near_norm, jitter_norm);

#if DSP_DIAG
	diag_blocks++;
	diag_mag_sum += mag_norm;
	diag_side_sum += side_norm;
	diag_smax_sum += side_max_norm;
	diag_near_sum += near_norm;
	diag_jit_sum += jitter_norm;
	if (mag_norm > diag_mag_max) diag_mag_max = mag_norm;
	if (side_max_norm > diag_smax_max) diag_smax_max = side_max_norm;
#endif

	return mag_norm;
}

//==================================================================
//	スペクトラム (256pt FFT + Hann窓)
//==================================================================
static void process_spectrum(void)
{
	static float fr[DSP_SPEC_N];
	static float fi[DSP_SPEC_N];
	uint16_t local[DSP_SPEC_BINS + 1];

	uint16_t base = (uint16_t)((sample_pos + 512 - DSP_SPEC_N) % 512);
	float mean = 0.0f;
	for (int i = 0; i < DSP_SPEC_N; i++) {
		fr[i] = (float)sample_ring[(base + i) % 512];
		mean += fr[i];
	}
	mean /= (float)DSP_SPEC_N;
	for (int i = 0; i < DSP_SPEC_N; i++) {
		fr[i] = (fr[i] - mean) * hann[i];
		fi[i] = 0.0f;
	}
	float_fft(fr, fi, DSP_SPEC_LOG2);

	int max_i = 0;
	float max_m = 0.0f;
	float mags[DSP_SPEC_BINS + 1];
	for (int i = 0; i <= DSP_SPEC_BINS; i++) {
		float m = sqrtf(fr[i] * fr[i] + fi[i] * fi[i]);
		mags[i] = m;
		local[i] = (m > 65535.0f) ? 65535 : (uint16_t)m;
		// ピーク探索は表示帯域 (約300〜1200Hz) のみ
		if (i >= 10 && i <= 38 && m > max_m) {
			max_m = m;
			max_i = i;
		}
	}

	// AUTOモード: 550〜1000Hz (±1ビン強の探索マージン付き) の最強ピークへ
	// ゲート中心を自動同調する (手動TONEは600〜1000のまま)。
	// 注意: 500Hz台へロックすると下側サイドが167Hz付近に落ちるため、
	// 低域ノイズ環境ではスケルチが締まり感度が下がる場合がある。
	// - ピークが帯域内ノイズ床(ピーク±1ビン除外の平均)の3倍以上のとき
	//   だけ「信号」とみなす (ノイズの偶発ピークを追わない)
	// - 3フレーム(約100ms)連続で±20Hz以内に立ったときだけ引き込む
	// - ゲートON中(受信中)は±25Hzの微修正のみ許可 (局の乗り換え禁止)
	// - 信号が消えたら最後の周波数をホールド
#define DSP_AUTO_HZ_MIN 550
#define DSP_AUTO_HZ_MAX 1000
// AUTO引き込みの絶対床: 主判定は相対条件(ノイズ床3倍+3フレーム一致)で、
// これは無音時の誤ロックを防ぐ最低限の値 (PK表示のしきい値とは別)
	if (tone_sel == DSP_TONE_AUTO) {
		const int lo = 16;   // 500Hz (550Hz - 1.5bin)
		const int hi = 33;   // 1031.25Hz (1000Hz + 1bin)
		int bi = lo;
		float bm = 0.0f;
		for (int i = lo; i <= hi; i++) {
			if (mags[i] > bm) { bm = mags[i]; bi = i; }
		}
		float nf = 0.0f;
		int nn = 0;
		for (int i = lo; i <= hi; i++) {
			if (i >= bi - 1 && i <= bi + 1) continue;
			nf += mags[i];
			nn++;
		}
		nf = (nn > 0) ? (nf / (float)nn) : 0.0f;
#if DSP_DIAG
		{
			static uint32_t at_last_ms = 0;
			uint32_t at_now = millis();
			if ((at_now - at_last_ms) >= 1000) {
				at_last_ms = at_now;
				Serial.printf("[auto] bi=%d(%dHz) bm=%d nf=%d cand=%u cnt=%u gate_hz=%u dg=%d\n",
				              bi, (int)(bi * 31.25f), (int)bm, (int)nf,
				              (unsigned)cand_hz, (unsigned)cand_cnt,
				              (unsigned)gate_hz, (int)decoder_gate());
			}
		}
#endif
		if (bm >= (float)DSP_AUTO_MIN_MAG && bm > nf * 3.0f && bi > lo && bi < hi) {
			float pa = mags[bi - 1], pb = mags[bi], pc = mags[bi + 1];
			float den = pa - 2.0f * pb + pc;
			float pd = (den != 0.0f) ? 0.5f * (pa - pc) / den : 0.0f;
			if (pd < -0.5f) pd = -0.5f;
			if (pd > 0.5f) pd = 0.5f;
			uint16_t fpk = (uint16_t)(((float)bi + pd) *
			               ((float)DSP_SAMPLE_RATE / (float)DSP_SPEC_N) + 0.5f);
			int cd = (int)fpk - (int)cand_hz;
			if (cand_cnt > 0 && cd >= -20 && cd <= 20) {
				cand_hz = (uint16_t)(((int)cand_hz + (int)fpk) / 2);
				if (cand_cnt < 100) cand_cnt++;
			} else {
				cand_hz = fpk;
				cand_cnt = 1;
			}
			if (cand_cnt >= 3) {
				uint16_t nh = cand_hz;
				if (nh < DSP_AUTO_HZ_MIN) nh = DSP_AUTO_HZ_MIN;
				if (nh > DSP_AUTO_HZ_MAX) nh = DSP_AUTO_HZ_MAX;
				int diff = (int)nh - (int)gate_hz;
				if (diff < 0) diff = -diff;
				if (diff > 5 && (diff <= 25 || !decoder_gate())) {
					if (diff > 100) {
						side_ema_started = 0;
					}
					gate_hz = nh;
					gate_update_coeff();
				}
			}
		} else {
			cand_cnt = 0;
		}
	}

	// ピーク表示もAUTO同調と同じ絶対床・SNR・3フレーム安定条件にする。
	// 探索帯域は表示用の約300〜1200Hz (bin 10〜38) を維持。
	float peak_nf = 0.0f;
	int peak_nn = 0;
	for (int i = 10; i <= 38; i++) {
		if (i >= max_i - 1 && i <= max_i + 1) continue;
		peak_nf += mags[i];
		peak_nn++;
	}
	if (peak_nn > 0) peak_nf /= (float)peak_nn;

	uint16_t pk = 0;
	if (max_m >= (float)DSP_AUTO_MIN_MAG && max_m > peak_nf * 3.0f &&
	    max_i > 10 && max_i < 38) {
		// 放物線補間でビン間周波数を推定
		float a = mags[max_i - 1], b = mags[max_i], c = mags[max_i + 1];
		float denom = a - 2.0f * b + c;
		float d = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;
		if (d < -0.5f) d = -0.5f;
		if (d > 0.5f) d = 0.5f;
		float f = ((float)max_i + d) * ((float)DSP_SAMPLE_RATE / (float)DSP_SPEC_N);
		uint16_t measured_hz = (uint16_t)(f + 0.5f);
		int diff = (int)measured_hz - (int)peak_cand_hz;
		if (peak_cand_cnt > 0 && diff >= -20 && diff <= 20) {
			peak_cand_hz = (uint16_t)(((int)peak_cand_hz + (int)measured_hz) / 2);
			if (peak_cand_cnt < 3) peak_cand_cnt++;
		} else {
			peak_cand_hz = measured_hz;
			peak_cand_cnt = 1;
		}
		if (peak_cand_cnt >= 3) pk = peak_cand_hz;
	} else {
		peak_cand_cnt = 0;
	}

	taskENTER_CRITICAL(&dsp_mux);
	memcpy(spec_mag, local, sizeof(spec_mag));
	peak_hz = pk;
	taskEXIT_CRITICAL(&dsp_mux);
}

//==================================================================
//	DSPタスク本体
//==================================================================
static void dsp_task(void *arg)
{
	static uint16_t raw[DSP_HOP];
	uint8_t spec_div = 0;
	uint16_t col_acc_q8 = 0;
	int16_t col_mn = 32767, col_mx = -32768;
	uint16_t col_m1 = 0, col_m2 = 0;
	uint8_t col_gate_cnt = 0;
	uint8_t col_hops = 0;
#if DSP_DIAG
	uint32_t diag_last_ms = millis();
#endif

	for (;;) {
		size_t got = audio_read(raw, DSP_HOP);
		if (got == 0) continue;

		int16_t mn = 32767, mx = -32768;
		for (size_t i = 0; i < got; i++) {
			int32_t r = (int32_t)raw[i] << 8;             // Q8
			dc_est += (r - dc_est) >> 10;                 // ゆっくり直流追従
			int16_t s = (int16_t)((r - dc_est) >> 8);       // ±2048 (中心値がフルスケール中央の場合)
			sample_ring[sample_pos] = s;
			sample_pos = (uint16_t)((sample_pos + 1) % 512);
			if (s < mn) mn = s;
			if (s > mx) mx = s;
		}

		// 入力レベル(絶対): フルスケール ±2048 に対する%。
		// ピーク保持+緩降下(時定数~0.2s)。100%付近はADCクリップの目安。
		{
			int32_t amp = (mx > -mn) ? mx : -mn;
			static int32_t ipk = 0;
			if (amp > ipk) ipk = amp;
			else ipk -= (ipk >> 5) + 1;
			if (ipk < 0) ipk = 0;
			int32_t p = ipk * 100 / 2048;
			input_pct = (uint8_t)((p > 100) ? 100 : p);
		}

		int32_t mag = process_gate();
		uint16_t mag16 = (mag > 65535) ? 65535 : (uint16_t)((mag < 0) ? 0 : mag);

		// ゲート窓長のWPM追従 (ヒステリシス付き)。切替時はサイドEMAを
		// リセット (窓長で振幅スケールが変わるため)。適応しきい値は
		// 数十msで自動追従する
		{
			uint16_t w = decoder_wpm();
			uint8_t desired = gate_win;
			if (w >= 32) {
				desired = DSP_HOP;           // 48 (6ms窓、v1.9同等)
			} else if (w != 0 && w <= 28) {
				desired = DSP_GATE_WIN;      // 96 (12ms窓、+3dB)
			}
			if (desired != gate_win) {
				gate_win = desired;
				side_ema_started = 0;
			}
		}

		// スコープ列: WPM追従の可変hop数をまとめて1列 (Q8で分数蓄積)
		if (mn < col_mn) col_mn = mn;
		if (mx > col_mx) col_mx = mx;
		if (mag16 > col_m1) {
			col_m2 = col_m1;
			col_m1 = mag16;
		} else if (mag16 > col_m2) {
			col_m2 = mag16;
		}
		col_gate_cnt = (uint8_t)(col_gate_cnt + decoder_gate());
		col_hops++;
		col_acc_q8 += 256;
		if (col_acc_q8 >= scope_period_q8) {
			col_acc_q8 -= scope_period_q8;
			taskENTER_CRITICAL(&dsp_mux);
			scope_col_t *col = &scope_ring[scope_pos];
			col->mn = col_mn;
			col->mx = col_mx;
			// エンベロープは「2番目に大きい値」(2hop未満は最大値):
			// LCD転送バースト等の1ブロック限りの混入スパイクを表示から除去
			col->mag = (col_hops >= 2) ? col_m2 : col_m1;
			// KEY は多数決 (半数以上ONで列ON): OR だと短い要素間ギャップが
			// 飲み込まれ符号パターンに見えなくなる
			col->gate = ((uint16_t)col_gate_cnt * 2 >= col_hops) ? 1 : 0;
			scope_pos = (uint16_t)((scope_pos + 1) % SCOPE_RING_SIZE);
			taskEXIT_CRITICAL(&dsp_mux);
			col_mn = 32767;
			col_mx = -32768;
			col_m1 = 0;
			col_m2 = 0;
			col_gate_cnt = 0;
			col_hops = 0;
			// 次列の周期をWPMから更新 (20WPM以下=2.0hop、40WPM=1.0hop)
			{
				uint16_t w = decoder_wpm();
				if (w == 0) w = 20;
				uint32_t q = (60UL << 8) / w;
				if (q < SCOPE_HOPS_Q8_MIN) q = SCOPE_HOPS_Q8_MIN;
				if (q > SCOPE_HOPS_Q8_MAX) q = SCOPE_HOPS_Q8_MAX;
				scope_period_q8 = (uint16_t)q;
			}
		}

		if (++spec_div >= SPEC_INTERVAL_HOPS) {
			spec_div = 0;
			process_spectrum();
		}

#if DSP_DIAG
		{
			static int16_t diag_raw_mn = 32767;
			static int16_t diag_raw_mx = -32768;
			static uint8_t diag_spec_div = 0;
			if (mn < diag_raw_mn) diag_raw_mn = mn;
			if (mx > diag_raw_mx) diag_raw_mx = mx;
			uint32_t now = millis();
			if ((now - diag_last_ms) >= 1000) {
				diag_last_ms = now;
				uint32_t n = (diag_blocks > 0) ? diag_blocks : 1;
				Serial.printf("[dsp] blk/s=%u mag avg=%d max=%d side=%d near=%d jit=%d smax=%d limit=%d gate=%d hz=%u\n",
				              (unsigned)diag_blocks,
				              (int)(diag_mag_sum / n), (int)diag_mag_max,
				              (int)(diag_side_sum / n),
				              (int)(diag_near_sum / n),
				              (int)(diag_jit_sum / n),
				              (int)(diag_smax_sum / n),
				              (int)decoder_maglimit(), (int)decoder_gate(),
				              (unsigned)gate_hz);
				diag_blocks = 0;
				diag_mag_sum = 0;
				diag_side_sum = 0;
				diag_smax_sum = 0;
				diag_near_sum = 0;
				diag_jit_sum = 0;
				diag_mag_max = 0;
				diag_smax_max = 0;
				diag_raw_mn = 32767;
				diag_raw_mx = -32768;
				// 5秒毎にスペクトラム概形 (bin0〜63 を4bin毎に平均、/64縮小)
				if (++diag_spec_div >= 5) {
					diag_spec_div = 0;
					uint16_t sp[DSP_SPEC_BINS + 1];
					dsp_get_spectrum(sp);
					Serial.print("[dsp] spec/64:");
					for (int i = 0; i < 64; i += 4) {
						uint32_t s = ((uint32_t)sp[i] + sp[i + 1] + sp[i + 2] + sp[i + 3]) / 4;
						Serial.printf(" %u", (unsigned)(s >> 6));
					}
					Serial.println();
				}
			}
		}
#endif
	}
}

void dsp_start(void)
{
	gate_update_coeff();
	for (int i = 0; i < DSP_SPEC_N; i++) {
		hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(DSP_SPEC_N - 1)));
	}
	memset(scope_ring, 0, sizeof(scope_ring));
	memset(spec_mag, 0, sizeof(spec_mag));
	xTaskCreatePinnedToCore(dsp_task, "dsp", 8192, NULL, 3, NULL, 0);
}
