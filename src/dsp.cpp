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
#define SCOPE_DECIM 2           // スコープ1列 = 2 hop (12ms、約1.8s/画面)
#define SPEC_INTERVAL_HOPS 6    // 36ms 毎 (約28fps)
#define DSP_PEAK_MIN 2400       // ピーク周波数表示のしきい値
#define DSP_DIAG 0              // 毎秒 blk/s・mag統計をシリアル出力

#if DSP_DIAG
static uint32_t diag_blocks = 0;
static int32_t diag_mag_max = 0;
static int64_t diag_mag_sum = 0;
static int64_t diag_side_sum = 0;
static int64_t diag_smax_sum = 0;
static int32_t diag_smax_max = 0;
#endif

static const uint16_t tone_tbl[DSP_TONE_COUNT] = { 600, 700, 800, 900, 1000 };
static volatile uint8_t tone_sel = DSP_TONE_AUTO;   // デフォルトはAUTO
static volatile uint16_t gate_hz = 700;             // ゲート中心 (AUTO待機時は帯域中央)

// AUTO同調の候補追跡
static uint16_t cand_hz = 0;
static uint8_t cand_cnt = 0;

static int16_t sample_ring[512];
static uint16_t sample_pos = 0;
static int32_t dc_est = 2048 * 256;   // 生ADC値の直流分 (Q8)

static scope_col_t scope_ring[SCOPE_RING_SIZE];
static uint16_t scope_pos = 0;

static uint16_t spec_mag[DSP_SPEC_BINS + 1];
static uint16_t peak_hz = 0;
static float hann[DSP_SPEC_N];

static int32_t side_ema_l = 0;
static int32_t side_ema_h = 0;
static uint8_t side_ema_started = 0;

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
static float g_coeff_l = 0.0f;
static float g_coeff_h = 0.0f;

static void gate_update_coeff(void)
{
	float fc = (float)gate_hz;
	float fstep = (float)DSP_SAMPLE_RATE / (float)DSP_HOP;   // 166.67Hz
	g_coeff_c = 2.0f * cosf(2.0f * (float)M_PI * fc / (float)DSP_SAMPLE_RATE);
	g_coeff_l = 2.0f * cosf(2.0f * (float)M_PI * (fc - 2.0f * fstep) / (float)DSP_SAMPLE_RATE);
	g_coeff_h = 2.0f * cosf(2.0f * (float)M_PI * (fc + 2.0f * fstep) / (float)DSP_SAMPLE_RATE);
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
	// 直近 DSP_GATE_WIN サンプルを窓として使う (ホップ48で50%重複)
	float win[DSP_GATE_WIN];
	float mean = 0.0f;
	uint16_t base = (uint16_t)((sample_pos + 512 - DSP_GATE_WIN) % 512);
	for (int i = 0; i < DSP_GATE_WIN; i++) {
		win[i] = (float)sample_ring[(base + i) % 512];
		mean += win[i];
	}
	mean /= (float)DSP_GATE_WIN;

	float q1c = 0.0f, q2c = 0.0f;
	float q1l = 0.0f, q2l = 0.0f;
	float q1h = 0.0f, q2h = 0.0f;
	for (int i = 0; i < DSP_GATE_WIN; i++) {
		const float x = win[i] - mean;
		float q0;
		q0 = g_coeff_c * q1c - q2c + x; q2c = q1c; q1c = q0;
		q0 = g_coeff_l * q1l - q2l + x; q2l = q1l; q1l = q0;
		q0 = g_coeff_h * q1h - q2h + x; q2h = q1h; q1h = q0;
	}

	int32_t mag_c = goertzel_mag(q1c, q2c, g_coeff_c);
	int32_t mag_l = goertzel_mag(q1l, q2l, g_coeff_l);
	int32_t mag_h = goertzel_mag(q1h, q2h, g_coeff_h);

	int32_t side_inst = (mag_l < mag_h) ? mag_l : mag_h;
	int32_t side_inst_max = (mag_l > mag_h) ? mag_l : mag_h;
	if (!side_ema_started) {
		side_ema_started = 1;
		side_ema_l = mag_l;
		side_ema_h = mag_h;
	} else {
		side_ema_l += (mag_l - side_ema_l) / 4;
		side_ema_h += (mag_h - side_ema_h) / 4;
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

	decoder_process_block(mag_norm, side_norm, side_inst_norm, side_max_norm);

#if DSP_DIAG
	diag_blocks++;
	diag_mag_sum += mag_norm;
	diag_side_sum += side_norm;
	diag_smax_sum += side_max_norm;
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

	// AUTOモード: 600〜1000Hz (±1ビンの探索マージン付き) の最強ピークへ
	// ゲート中心を自動同調する。
	// - ピークが帯域内ノイズ床(ピーク±1ビン除外の平均)の3倍以上のとき
	//   だけ「信号」とみなす (ノイズの偶発ピークを追わない)
	// - 3フレーム(約100ms)連続で±20Hz以内に立ったときだけ引き込む
	// - ゲートON中(受信中)は±25Hzの微修正のみ許可 (局の乗り換え禁止)
	// - 信号が消えたら最後の周波数をホールド
	if (tone_sel == DSP_TONE_AUTO) {
		const int lo = 18;   // 562.5Hz
		const int hi = 33;   // 1031.25Hz
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
		if (bm >= (float)DSP_PEAK_MIN && bm > nf * 3.0f && bi > lo && bi < hi) {
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
				if (nh < tone_tbl[0]) nh = tone_tbl[0];
				if (nh > tone_tbl[DSP_TONE_COUNT - 1]) nh = tone_tbl[DSP_TONE_COUNT - 1];
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

	uint16_t pk = 0;
	if (max_m >= (float)DSP_PEAK_MIN && max_i > 4 && max_i < DSP_SPEC_BINS) {
		// 放物線補間でビン間周波数を推定
		float a = mags[max_i - 1], b = mags[max_i], c = mags[max_i + 1];
		float denom = a - 2.0f * b + c;
		float d = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;
		if (d < -0.5f) d = -0.5f;
		if (d > 0.5f) d = 0.5f;
		float f = ((float)max_i + d) * ((float)DSP_SAMPLE_RATE / (float)DSP_SPEC_N);
		pk = (uint16_t)(f + 0.5f);
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
	uint8_t scope_phase = 0;
	int16_t col_mn = 32767, col_mx = -32768;
	uint16_t col_mags[SCOPE_DECIM] = { 0 };
	uint8_t col_gate = 0;
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
			int16_t s = (int16_t)(((r - dc_est) >> 8) / 2); // ±1024 (CH32版相当の検出感度)
			sample_ring[sample_pos] = s;
			sample_pos = (uint16_t)((sample_pos + 1) % 512);
			if (s < mn) mn = s;
			if (s > mx) mx = s;
		}

		int32_t mag = process_gate();
		uint16_t mag16 = (mag > 65535) ? 65535 : (uint16_t)((mag < 0) ? 0 : mag);

		// スコープ列: SCOPE_DECIM hop 分をまとめて1列
		if (mn < col_mn) col_mn = mn;
		if (mx > col_mx) col_mx = mx;
		col_mags[scope_phase] = mag16;
		col_gate = (uint8_t)(col_gate + decoder_gate());
		if (++scope_phase >= SCOPE_DECIM) {
			// エンベロープは「2番目に大きい値」を採用: LCD転送バースト等の
			// 1ブロック限りの混入スパイクを表示から除去する
			// (実信号のマークは全ブロックが高いので影響しない)
			uint16_t m1 = 0, m2 = 0;
			for (uint8_t i = 0; i < SCOPE_DECIM; i++) {
				if (col_mags[i] > m1) {
					m2 = m1;
					m1 = col_mags[i];
				} else if (col_mags[i] > m2) {
					m2 = col_mags[i];
				}
			}
			taskENTER_CRITICAL(&dsp_mux);
			scope_col_t *col = &scope_ring[scope_pos];
			col->mn = col_mn;
			col->mx = col_mx;
			col->mag = m2;
			// KEY は多数決 (4ブロック中2以上ONで列ON): OR だと短い要素間
			// ギャップ(<24ms超過分)が飲み込まれ符号パターンに見えなくなる
			col->gate = (col_gate >= SCOPE_DECIM / 2) ? 1 : 0;
			scope_pos = (uint16_t)((scope_pos + 1) % SCOPE_RING_SIZE);
			taskEXIT_CRITICAL(&dsp_mux);
			scope_phase = 0;
			col_mn = 32767;
			col_mx = -32768;
			for (uint8_t i = 0; i < SCOPE_DECIM; i++) col_mags[i] = 0;
			col_gate = 0;
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
				Serial.printf("[dsp] blk/s=%u raw=%d..%d mag avg=%d max=%d side avg=%d smax avg=%d max=%d limit=%d\n",
				              (unsigned)diag_blocks,
				              (int)diag_raw_mn, (int)diag_raw_mx,
				              (int)(diag_mag_sum / n), (int)diag_mag_max,
				              (int)(diag_side_sum / n),
				              (int)(diag_smax_sum / n), (int)diag_smax_max,
				              (int)decoder_maglimit());
				diag_blocks = 0;
				diag_mag_sum = 0;
				diag_side_sum = 0;
				diag_smax_sum = 0;
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
