//
//	DSP タスク (Core 0)
//	- ADC DMA から 30サンプル(3.75ms)単位で取得
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
#define SCOPE_HOPS_Q8_MIN 768   // 3.0 hop = 11ms (40WPM)
#define SCOPE_HOPS_Q8_MAX 1536  // 6.0 hop = 22ms (20WPM以下)
#define SPEC_INTERVAL_HOPS 12   // 45ms 毎 (約22fps)
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
#define DSP_AUTO_IDLE_HZ 600                        // AUTO で信号を掴んでいない間のゲート中心
static volatile uint16_t gate_hz = DSP_AUTO_IDLE_HZ;   // ゲート中心

// AUTO同調の候補追跡
static uint16_t cand_hz = 0;
static uint8_t cand_cnt = 0;
// ロック中周波数の FFT ピークホールド (フレーム毎に 3% 減衰)。
// 隙間のノイズピークへの乗り換えを防ぐ: 無条件追従の範囲を超える乗り換えは
// 候補がこれの 1.5 倍以上のときだけ許す。局が消えれば 1〜2 秒で減衰して次を掴む
static float lock_hold = 0.0f;
// 無条件追従の範囲。「信号が検出帯域の端に半分かかった状態から引き戻す」のが
// 目的なので、ゲートの半値幅 (帯域 67Hz の半分 = ±33Hz) に合わせる。
// ここを半値幅より広く取ると、符号間・文字間・語間 (ゲートOFF) に立った
// ノイズのピークへ無条件で乗り換えてしまい、ゲートが信号から完全に外れる。
// 実測 (ノイズ混じりの録音): 70Hz のとき 0.6 秒間 600→650Hz へ飛び、その間の
// 文字が 19/34ms の断片としてしか拾えず「短すぎ」で捨てられていた。
// 35Hz にすると飛びが消え、正解率 97% → 99% (他の条件に退行なし)
#define DSP_AUTO_NEAR_HZ 35          // これ以下のズレはゲートOFF時に無条件で追従
#define DSP_AUTO_STEAL_RATIO 1.5f    // 遠くへ乗り換える条件: 候補 > ホールド x これ
// 付け替えの不感帯: ずれがこれ以下なら同調先を動かさない
#define DSP_AUTO_DEADBAND_HZ 5
// ゲート ON (受信中) でも許す微修正の幅
#define DSP_AUTO_ON_TRACK_HZ 25
static uint16_t peak_cand_hz = 0;
static uint8_t peak_cand_cnt = 0;

static int16_t sample_ring[512];
static int16_t ale_ring[512];           // ALE の出力 y を sample_ring と同じ位置に積む
static uint8_t det_mode = DSP_DET_GOERTZEL;            // DSP タスクが使う方式
static volatile uint8_t det_req = DSP_DET_GOERTZEL;    // 切替要求 (次のブロックで反映)
// ALE のタップ数 L と予測遅延 D。DSP_DET_ALE_AMP の評価で決めた値
// (L=128 は 40WPM の短点をなまらせて 94% → 75%、L=64 で A と同等)
static uint16_t ale_taps = 64;
static uint16_t ale_delay = 16;
static uint16_t sample_pos = 0;
static int32_t dc_est = 2048 * 256;   // 生ADC値の直流分 (Q8)

static scope_col_t scope_ring[SCOPE_RING_SIZE];
static uint16_t scope_pos = 0;
static volatile uint32_t scope_total = 0;   // 生成したスコープ列の通し番号 (文字の時間軸位置用)
// デコード中の文字の符号区間 (スコープ列番号): 前回の文字確定後、最初に
// ゲートが ON になった列 〜 最後に OFF になった列。文字をその中央に置く
static volatile uint32_t span_start = 0;
static volatile uint32_t span_end = 0;
static volatile uint8_t span_open = 0;
static volatile uint16_t scope_period_q8 = 1536;  // 現在の1列hop数 (Q8)
static volatile uint8_t input_pct = 0;            // 入力レベル (フルスケール比%)
static volatile int16_t input_peak = 0;           // 入力ピーク振幅 (カウント、ピークホールド)

static uint16_t spec_mag[DSP_SPEC_BINS + 1];
static uint16_t peak_hz = 0;
static float hann[DSP_SPEC_N];

static int32_t side_ema_l = 0;
static int32_t side_ema_h = 0;
static uint8_t side_ema_started = 0;

// トーン判定窓長 (サンプル数)。WPM追従で切替:
// 15ms窓(120)はエッジが±7msなまり、50WPM超(ギャップ24ms未満)では要素間
// ギャップが20ms未満に潰れて短点が融合する。高速時は v1.9 実証済みの
// 7.5ms窓(60)へ戻す。ヒステリシス: ≥45WPMで60 / ≤40WPMで120。
// ホップを 3.75ms にしたことで長窓のまま 50WPM 付近まで持つ (旧: 32WPM)。
static volatile uint8_t gate_win = DSP_GATE_WIN_LONG;

// 直近ブロックのトーン判定値 (スコープログ用)
static volatile int32_t last_side_norm = 0;
static volatile int32_t last_near_norm = 0;

static portMUX_TYPE dsp_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t dsp_paused = 0;
static volatile uint8_t dsp_idle = 0;       // 1=一時停止中でタスクが audio_read を呼んでいない
static volatile uint8_t dsp_alive = 0;

static void gate_update_coeff(void);

void dsp_set_tone(uint8_t idx)
{
	if (idx > DSP_TONE_AUTO) idx = 0;
	tone_sel = idx;
	if (idx != DSP_TONE_AUTO) {
		gate_hz = tone_tbl[idx];
	} else {
		// AUTO は待機周波数から探し直す (手動 1000Hz から AUTO へ送った
		// ときに 1000Hz へ居座らせない)。ピークホールドも捨てないと、
		// 直前の手動周波数のホールドが残って引き込みが 1〜2 秒遅れる
		gate_hz = DSP_AUTO_IDLE_HZ;
		lock_hold = 0.0f;
	}
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

uint32_t dsp_scope_col_index(void)
{
	return scope_total;
}

uint16_t dsp_scope_period_q8(void)
{
	return scope_period_q8;
}

int dsp_get_scope_since(uint32_t from_idx, scope_col_t *out, int max,
                        uint32_t *first, uint32_t *lost)
{
	taskENTER_CRITICAL(&dsp_mux);
	uint32_t total = scope_total;
	uint32_t oldest = (total > SCOPE_RING_SIZE) ? total - SCOPE_RING_SIZE : 0;
	uint32_t start = from_idx;
	*lost = 0;
	if (start < oldest) {
		*lost = oldest - start;
		start = oldest;
	}
	int n = 0;
	for (uint32_t i = start; i < total && n < max; i++, n++) {
		out[n] = scope_ring[i % SCOPE_RING_SIZE];
	}
	taskEXIT_CRITICAL(&dsp_mux);
	*first = start;
	return n;
}

void dsp_scope_char_span(uint32_t *start, uint32_t *end)
{
	*start = span_start;
	*end = span_end;
	span_open = 0;                  // 次の文字の区間を新しく開く
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
	// 1 hop = DSP_HOP/8kHz (3.75ms) を 0.1ms 単位で。1/1000ms 単位で掛けて丸める
	const uint32_t hop_us = (uint32_t)DSP_HOP * 1000000UL / DSP_SAMPLE_RATE;   // 3750
	return (uint16_t)((((uint32_t)scope_period_q8 * hop_us) >> 8) / 100U);
}

uint16_t dsp_gate_bw_hz(void)
{
	return (uint16_t)(DSP_SAMPLE_RATE / gate_win);
}

uint8_t dsp_input_level_pct(void)
{
	return input_pct;
}

int16_t dsp_input_peak(void)
{
	return input_peak;
}

//==================================================================
//	トーン判定: 3ビン float Goertzel (CH32版 v1.8/1.9 と同一方式)
//	窓長は 120 サンプル (15ms)、ホップ 30 (判定周期 3.75ms、75%重複)。
//	帯域幅 66.7Hz と CH32版(167Hz)の約4割で、帯域内ノイズ電力が
//	減る = SNR +3.9dB。重畳バンドノイズ下の弱信号対策。
//	代償: トーン同調が中心±33Hz程度までシビアになる。
//	サイド = ±267Hz は120サンプル窓では±4 DFTビンにあたり、
//	引き続き矩形窓 Dirichlet核のヌル上 (純音はサイドへ漏れない)。
//	(値は v2.0 実機調整時の物理条件を再現したもの。dsp.h DSP_HOP 参照)
//	(FFTゲートはヌル配置が崩れトーンの周波数ズレに弱く、実機で
//	 デコード率が劣化したため廃止。FFT はスペアナ表示専用)
//	戻り値: デコーダ正規化後の中心マグニチュード
//==================================================================
static float g_coeff_c = 0.0f;
static float g_coeff_l = 0.0f;    // 遠サイド -2 hopビン = -271Hz (±2/±4 bin)
static float g_coeff_h = 0.0f;    // 遠サイド +271Hz
static float g_coeff_nl = 0.0f;   // 近サイド -1 hopビン = -136Hz (±1/±2 bin)
static float g_coeff_nh = 0.0f;   // 近サイド +136Hz

// 近サイド用EMA + 包絡線ジッタ
static int32_t near_ema_l = 0;
static int32_t near_ema_h = 0;
static int32_t jitter_ema = 0;
static int32_t mag_prev = 0;

static void gate_update_coeff(void)
{
	float fc = (float)gate_hz;
	// サイドの間隔は短窓のヌル間隔 = fs/60 = 133.3Hz (長窓ではその2ビンぶん)
	float fstep = (float)DSP_SAMPLE_RATE / (float)DSP_GATE_WIN_SHORT;   // 133.3Hz
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

//	中心ビンは生入力と ALE 出力 (y) の 2 本を計算できる。サイドビンは常に
//	生入力から取る: サイドまで y から取ると、ALE が狭い BPF として作る
//	スペクトルの傾きを純音と取り違え、ノイズで偽符号が出る (実測 8 個/分)。
//	どちらの中心をどこに使うかは det_mode で決める:
//	  振幅 (しきい値・ノイズ床) 用 / 純音判定 (サイド比) 用
//	  GOERTZEL     生 / 生
//	  ALE_GOERTZEL y  / y
//	  ALE_TONE     生 / y   … 感度は現行のまま、純音判定だけ ALE で厳しく
//	  ALE_AMP      y  / 生
static int32_t process_gate(void)
{
	// 直近 gate_win サンプルを窓として使う (120時はホップ30で75%重複)
	const int n = gate_win;
	const uint8_t use_ale = (det_mode != DSP_DET_GOERTZEL);
	float win[DSP_GATE_WIN_LONG];
	float win_a[DSP_GATE_WIN_LONG];
	float mean = 0.0f, mean_a = 0.0f;
	uint16_t base = (uint16_t)((sample_pos + 512 - n) % 512);
	// y の包絡線は生入力より平均 D + L/2 サンプル遅れて立ち上がり、同じだけ
	// 遅れて消える。生入力側の窓をその分さかのぼって時間をそろえる。
	// そろえないと、振幅と純音判定で別の時刻を見ることになり、ON (両方
	// 成立) は遅れ OFF (片方で成立) は早まってマークが削られる
	// (実測: 振幅=生 / 純音=y で -3dB の正解率が 100% → 53%)
	const uint16_t lag = use_ale ? (uint16_t)(ale_delay + ale_taps / 2) : 0;
	uint16_t base_raw = (uint16_t)((sample_pos + 1024 - n - lag) % 512);
	for (int i = 0; i < n; i++) {
		win[i] = (float)sample_ring[(base_raw + i) % 512];
		win_a[i] = use_ale ? (float)ale_ring[(base + i) % 512] : 0.0f;
		mean += win[i];
		mean_a += win_a[i];
	}
	mean /= (float)n;
	mean_a /= (float)n;

	float q1c = 0.0f, q2c = 0.0f;
	float q1a = 0.0f, q2a = 0.0f;
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
	if (use_ale) {
		for (int i = 0; i < n; i++) {
			float q0 = g_coeff_c * q1a - q2a + (win_a[i] - mean_a);
			q2a = q1a;
			q1a = q0;
		}
	}

	int32_t mag_c = goertzel_mag(q1c, q2c, g_coeff_c);
	int32_t mag_a = use_ale ? goertzel_mag(q1a, q2a, g_coeff_c) : mag_c;
	int32_t mag_l = goertzel_mag(q1l, q2l, g_coeff_l);
	int32_t mag_h = goertzel_mag(q1h, q2h, g_coeff_h);
	int32_t mag_nl = goertzel_mag(q1nl, q2nl, g_coeff_nl);
	int32_t mag_nh = goertzel_mag(q1nh, q2nh, g_coeff_nh);

	// 窓長によるスケール差を補正: 純音の Goertzel 出力は窓長に比例するので、
	// 短窓 (59) では x2 して長窓 (118) と同じ尺度にする。これが無いと窓切替の
	// 瞬間にトーン振幅が半分になり、しきい値 (limit) とノイズ床が旧尺度のまま
	// なので ON にならず、WPM が更新されなくなって短窓から戻れなくなる
	if (n < DSP_GATE_WIN_LONG) {
		const int32_t k = DSP_GATE_WIN_LONG / n;
		mag_c *= k; mag_a *= k; mag_l *= k; mag_h *= k; mag_nl *= k; mag_nh *= k;
	}

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
	const int32_t amp_src = (det_mode == DSP_DET_ALE_GOERTZEL || det_mode == DSP_DET_ALE_AMP) ? mag_a : mag_c;
	const int32_t tone_src = (det_mode == DSP_DET_ALE_GOERTZEL || det_mode == DSP_DET_ALE_TONE) ? mag_a : mag_c;
	int32_t mag_norm = amp_src >> 2;
	int32_t tone_norm = tone_src >> 2;
	int32_t side_norm = side >> 2;
	int32_t side_inst_norm = side_inst >> 2;
	int32_t side_max_norm = side_max >> 2;
	int32_t near_norm = near_side >> 2;
	int32_t jitter_norm = jitter_ema >> 2;

	last_side_norm = side_norm;
	last_near_norm = near_norm;
	decoder_process_block(mag_norm, tone_norm, side_norm, side_inst_norm, side_max_norm,
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
//	適応型ラインエンハンサ (ALE) — 評価中
//
//	NLMS の線形予測器。D サンプル遅らせた入力の L タップから現在値を
//	予測する。ノイズは D 以上離れると相関が無い (入力 BPF Q=3 の相関時間は
//	約 1.4ms = 11 サンプル) ので予測できず、周期的なトーンだけが予測できる。
//	  y = 予測 (トーンを強めた音声)
//	  e = x - y (予測できなかった成分 = ノイズ)
//	収束すると係数はトーン周波数に合った帯域 fs/L 程度の BPF になる。
//	NLMS の収束時定数はモードの電力比で決まり、トーンが来ると
//	約 2(1+1/SNR)/mu サンプルで掴み、ノイズだけの間は L/mu サンプルかけて
//	ゆっくり崩れる (L=128, mu=0.01 で 1.6 秒)。つまり符号間や語間では
//	係数がほぼ保たれ、次のキーダウンで即座に応答する。
//
//	判定方式 (dsp_set_det_mode):
//	  DSP_DET_ALE_GOERTZEL … 中心ビンだけ y から取り、サイドビンは生入力のまま
//	  DSP_DET_ALE_RATIO    … y の電力 / e の電力 (長時間平均) だけで ON/OFF。
//	                         Goertzel を使わない
//==================================================================
#define ALE_TAPS_MAX 256
#define ALE_HIST 512                    // 履歴リング (L + D 以上の 2 のべき)
static float ale_mu = 0.01f;
// 比しきい値。純ノイズでの比はマーク外の 99% 点で約 0.3 (L=128, D=32)。
// 合成 -6dB / 実ノイズの偽文字の釣り合いで決めた (web/tools/aletest.mjs)
static float ale_r_on = 0.45f;          // ON: Py / Pe_slow がこれを超える
static float ale_r_off = 0.25f;         // OFF: これを下回る
static float ale_w[ALE_TAPS_MAX];
static float ale_x[ALE_HIST];
static uint16_t ale_pos = 0;
static float ale_pow = 0.0f;            // 入力電力の長時間平均 (サンプル当たり)
static float ale_hop_y = 0.0f, ale_hop_e = 0.0f;   // 現ホップの二乗和
static float ale_win_y[DSP_GATE_WIN_LONG / DSP_HOP];   // 直近ホップの二乗和 (窓)
static uint8_t ale_win_pos = 0;
static float ale_e_slow = 0.0f;         // e の電力の長時間平均 (ホップ単位)
static uint8_t ale_e_started = 0;
static volatile float ale_last_ratio = 0.0f;
static float ale_peak = 0.0f;           // 比の振幅 (sqrt) のピーク追従

static void ale_reset(void)
{
	memset(ale_w, 0, sizeof(ale_w));
	memset(ale_x, 0, sizeof(ale_x));
	memset(ale_ring, 0, sizeof(ale_ring));
	memset(ale_win_y, 0, sizeof(ale_win_y));
	ale_pos = 0;
	ale_pow = 0.0f;
	ale_hop_y = ale_hop_e = 0.0f;
	ale_win_pos = 0;
	ale_e_slow = 0.0f;
	ale_e_started = 0;
	ale_peak = 0.0f;
}

//	切替は要求だけ出し、DSP タスクが次のブロックの頭で反映する。
//	ESP32 ではタッチ処理 (Core 1) から呼ばれるので、ここで ALE の状態を
//	初期化すると Core 0 で処理中の ale_sample() と競合する
void dsp_set_det_mode(uint8_t m)
{
	if (m > DSP_DET_ALE_AMP) m = DSP_DET_GOERTZEL;
	det_req = m;
}

uint8_t dsp_det_mode(void)
{
	return det_req;
}

void dsp_set_ale_param(uint16_t taps, uint16_t delay, float mu, float r_on, float r_off)
{
	if (taps < 8) taps = 8;
	if (taps > ALE_TAPS_MAX) taps = ALE_TAPS_MAX;
	if (delay < 1) delay = 1;
	if (taps + delay > ALE_HIST - 1) delay = (uint16_t)(ALE_HIST - 1 - taps);
	ale_taps = taps;
	ale_delay = delay;
	ale_mu = mu;
	ale_r_on = r_on;
	ale_r_off = r_off;
	ale_reset();
}

float dsp_ale_ratio(void)
{
	return ale_last_ratio;
}

//	1 サンプル処理して予測 y を返す。e の二乗和もここで積む
static inline float ale_sample(float x)
{
	const uint16_t L = ale_taps, D = ale_delay;
	const uint16_t M = ALE_HIST - 1;
	// 正規化は参照ベクトルの瞬時の二乗和ではなく、入力電力の長時間平均
	// (τ 0.5 秒) x L で行う。瞬時値だと、キーダウン直後は参照ベクトルに
	// まだトーンが入りきっておらず分母が小さいのに e はトーンまるごとで、
	// ステップが過大になって係数が跳ねる (実測: マーク頭で y が定常の
	// 2.7 倍に跳ね、その分マークが 20ms ほど長く測られた)
	ale_pow += (x * x - ale_pow) * (1.0f / 4096.0f);
	float y = 0.0f;
	for (uint16_t k = 0; k < L; k++) {
		y += ale_w[k] * ale_x[(uint16_t)(ale_pos - D - k) & M];
	}
	const float e = x - y;
	const float g = ale_mu * e / (ale_pow * (float)L + 1.0f);
	for (uint16_t k = 0; k < L; k++) {
		ale_w[k] += g * ale_x[(uint16_t)(ale_pos - D - k) & M];
	}
	ale_x[ale_pos & M] = x;
	ale_pos = (uint16_t)((ale_pos + 1) & M);
	ale_hop_y += y * y;
	ale_hop_e += e * e;
	return y;
}

//	DSP_DET_ALE_RATIO の判定 (ホップ毎)。窓は Goertzel と同じ gate_win
//	(既定 120 = 15ms) にそろえ、y の電力をその窓で、e の電力は τ 0.5 秒の
//	EMA で取る。e を長時間平均にするのは立ち上がりと立ち下がりの遅れを
//	そろえるため: y は D〜D+L サンプル遅れて立ち上がり、同じだけ遅れて
//	消える。e の短時間値を分母にすると、キーダウン直後 (y がまだ無く
//	e にトーンが残る) だけ比が下がり、マークが短く測られる
static int32_t ale_decide(void)
{
	const uint8_t nh = (uint8_t)(gate_win / DSP_HOP);
	ale_win_y[ale_win_pos] = ale_hop_y;
	ale_win_pos = (uint8_t)((ale_win_pos + 1) % (DSP_GATE_WIN_LONG / DSP_HOP));
	float py = 0.0f;
	for (uint8_t i = 0; i < nh; i++) {
		py += ale_win_y[(ale_win_pos + (DSP_GATE_WIN_LONG / DSP_HOP) - 1 - i) %
		                (DSP_GATE_WIN_LONG / DSP_HOP)];
	}
	py /= (float)(nh * DSP_HOP);
	const float pe = ale_hop_e / (float)DSP_HOP;
	// e の基準はノイズだけを見たい。単純な EMA だと、キーダウン直後に
	// e へ残るトーン (y が D〜D+L 遅れて追いつくまで) とキーアップ後の
	// y の残響が混ざり、きれいな信号ではこれが支配的になる (実測: マーク
	// 頭で比 6.9 → 同じマークの中で 0.6 まで落ちた)。基準の 4 倍を超える
	// ホップは取り込まず、そのときは基準を 1% ずつだけ持ち上げる
	// (本当にノイズが増えたときは 0.25 秒ほどで倍になって追いつく)
	if (!ale_e_started) {
		ale_e_started = 1;
		ale_e_slow = pe;
	} else if (pe < ale_e_slow * 4.0f) {
		ale_e_slow += (pe - ale_e_slow) * (1.0f / 128.0f);
	} else {
		ale_e_slow *= 1.01f;
	}
	ale_hop_y = ale_hop_e = 0.0f;

	const float r = py / (ale_e_slow + 1.0f);
	ale_last_ratio = r;
	// 固定の比しきい値だけだと、強い信号ほどキーアップ後の y の残響
	// (D+L サンプル) がしきい値を超えたまま残り、マークが伸びて空白が
	// 詰まる (10dB で語間が消えた)。現行の magnitudelimit と同じく、
	// 振幅 (sqrt) のピークに追従するしきい値を併用してエッジを信号の
	// 大きさによらず振幅の 0.6 / 0.4 倍の位置で切る
	const float a = sqrtf(r);
	if (a > ale_peak) {
		ale_peak += (a - ale_peak) * (1.0f / 6.0f);
	} else {
		ale_peak += (a - ale_peak) * (1.0f / 48.0f);
	}
	const uint8_t on = (r > ale_r_on) && (a * 5.0f > ale_peak * 3.0f);
	const uint8_t off = (r < ale_r_off) || (a * 5.0f < ale_peak * 2.0f);
	const int32_t mag = (int32_t)(a * 1000.0f);
	decoder_process_key(mag, (int32_t)(ale_peak * 1000.0f), on, off);
	return mag;
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

	// AUTOモード: 500〜1000Hz (±1ビン強の探索マージン付き) の最強ピークへ
	// ゲート中心を自動同調する (手動TONEは600〜1000のまま)。
	// 注意: 500Hz台へロックすると下側サイドが230Hz付近に落ちるため、
	// 低域ノイズ環境ではスケルチが締まり感度が下がる場合がある。
	// - ピークが帯域内ノイズ床(ピーク±1ビン除外の平均)の3倍以上のとき
	//   だけ「信号」とみなす (ノイズの偶発ピークを追わない)
	// - 3フレーム(約130ms)連続で±20Hz以内に立ったときだけ引き込む (待機時は 600Hz)
	// - ゲートON中(受信中)は±25Hzの微修正のみ許可 (局の乗り換え禁止)
	// - ゲートOFF時: ズレ 70Hz 以内 (帯域端に半分かかった信号) は追従、
	//   それ以上の乗り換えは候補がロック中周波数のピークホールドの 1.5 倍
	//   以上のときだけ。符号間・文字間・語間の隙間はゲートOFFなので、
	//   ゲート状態だけで判定すると隙間のノイズピークに乗り換えてしまう。
	//   (時間ホールド方式は帯域端の信号でゲートが断続ONになると永遠に
	//    引き込めなくなるため不採用)
	// - 信号が消えたら最後の周波数をホールド
#define DSP_AUTO_HZ_MIN 500
#define DSP_AUTO_HZ_MAX 1000
// AUTO引き込みの絶対床: 主判定は相対条件(ノイズ床3倍+3フレーム一致)で、
// これは無音時の誤ロックを防ぐ最低限の値 (PK表示のしきい値とは別)
	if (tone_sel == DSP_TONE_AUTO) {
		const int lo = 14;   // 437.5Hz (500Hz - 2bin)
		const int hi = 33;   // 1031.25Hz (1000Hz + 1bin)
		{
			int gb = (int)((float)gate_hz * (float)DSP_SPEC_N / (float)DSP_SAMPLE_RATE + 0.5f);
			float cur = (gb >= 0 && gb <= DSP_SPEC_BINS) ? mags[gb] : 0.0f;
			lock_hold *= 0.97f;
			if (cur > lock_hold) lock_hold = cur;
		}
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
				uint8_t steal_ok = (diff <= DSP_AUTO_NEAR_HZ) ||
				                   (lock_hold < (float)DSP_AUTO_MIN_MAG) ||
				                   (bm > lock_hold * DSP_AUTO_STEAL_RATIO);
				if (diff > DSP_AUTO_DEADBAND_HZ &&
				    (diff <= DSP_AUTO_ON_TRACK_HZ || (!decoder_gate() && steal_ok))) {
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
//	ブロック 1 回ぶん (DSP_HOP サンプル) の処理。
//	ESP32 では dsp_task() が無限ループで回し、Web (WASM) ビルドでは
//	AudioWorklet が供給できたぶんだけ外から dsp_step() を呼ぶ。
//	状態はステップ間で持ち越すので、元は dsp_task のローカルだったものを
//	すべて static にしてある。本体を do { ... } while (0) で囲ってあるので、
//	中の continue は「このステップを打ち切る」の意味のまま変わらない。
void dsp_step(void)
{
	static uint16_t raw[DSP_HOP];
	static uint8_t spec_div = 0;
	static uint16_t col_acc_q8 = 0;
	static int16_t col_mn = 32767, col_mx = -32768;
	static uint16_t col_m1 = 0, col_m2 = 0;
	static uint16_t col_mmin = 65535;
	static uint8_t col_gate_cnt = 0;
	static uint8_t col_on = 0, col_off = 0, col_real = 0;
	static uint8_t col_hops = 0;
#if DSP_DIAG
	static uint32_t diag_last_ms = 0;
#endif
	static uint64_t samples_total = 0;      // 取り込んだサンプル数 (ログの時間軸)

	do {
		if (dsp_paused) {
			// 一時停止中は ADC に触らない (呼び出し側が DMA を止めている)
			dsp_idle = 1;
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		dsp_idle = 0;
		size_t got = audio_read(raw, DSP_HOP);
		if (got == 0) continue;
		samples_total += got;

		if (det_req != det_mode) {
			ale_reset();
			side_ema_started = 0;
			det_mode = det_req;
		}

		int16_t mn = 32767, mx = -32768;
		for (size_t i = 0; i < got; i++) {
			int32_t r = (int32_t)raw[i] << 8;             // Q8
			dc_est += (r - dc_est) >> 10;                 // ゆっくり直流追従
			int16_t s = (int16_t)((r - dc_est) >> 8);       // ±2048 (中心値がフルスケール中央の場合)
			sample_ring[sample_pos] = s;
			if (det_mode != DSP_DET_GOERTZEL) {
				float y = ale_sample((float)s);
				ale_ring[sample_pos] = (int16_t)((y > 32767.0f) ? 32767.0f :
				                                 ((y < -32768.0f) ? -32768.0f : y));
			}
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
			input_peak = (int16_t)((ipk > 2048) ? 2048 : ipk);
		}

		int32_t mag;
		if (det_mode == DSP_DET_ALE_RATIO) {
			mag = ale_decide();
		} else {
			mag = process_gate();
		}
		uint16_t mag16 = (mag > 65535) ? 65535 : (uint16_t)((mag < 0) ? 0 : mag);
		{
			// 文字の符号区間の追跡 (デコード文字のスコープ表示用)
			static uint8_t gate_prev = 0;
			uint8_t g = decoder_gate();
			if (g && !gate_prev && !span_open) {
				span_open = 1;
				span_start = scope_total;
			}
			if (!g && gate_prev) {
				span_end = scope_total;
			}
			gate_prev = g;
		}

		// ゲート窓長のWPM追従 (ヒステリシス付き)。切替時はサイドEMAを
		// リセット (ノイズのスケールが √2 変わるため)。
		// ラッチアップ防止: 直近 2 秒ゲートONが無ければ WPM に関係なく長窓
		// (高感度側) へ戻す。短窓で信号を落とすと WPM が更新されず、
		// 「WPM ≥ 32 だから短窓」のまま固まってしまうため
		{
			static uint32_t blocks_since_on = 0;
			if (decoder_gate()) blocks_since_on = 0;
			else if (blocks_since_on < 0xFFFF) blocks_since_on++;
			const uint8_t stalled = blocks_since_on > (2000UL * DSP_SAMPLE_RATE / 1000 / DSP_HOP);
			uint16_t w = decoder_wpm();
			uint8_t desired = gate_win;
			if (stalled || (w != 0 && w <= 40)) {
				desired = DSP_GATE_WIN_LONG;   // 120 (15ms窓、+3dB)
			} else if (w >= 45) {
				desired = DSP_GATE_WIN_SHORT;  // 60 (7.5ms窓)
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
		if (mag16 < col_mmin) col_mmin = mag16;
		{
			uint8_t f = decoder_gate_flags();
			if (f & 1) col_on++;
			if (f & 2) col_off++;
			if (f & 4) col_real++;
		}
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
			col->t_ms = (uint32_t)(samples_total / (DSP_SAMPLE_RATE / 1000));
			col->side = (uint16_t)((last_side_norm > 65535) ? 65535 : last_side_norm);
			col->near = (uint16_t)((last_near_norm > 65535) ? 65535 : last_near_norm);
			{
				int32_t lm = decoder_maglimit(), nfv = decoder_noise_floor();
				col->limit = (uint16_t)((lm > 65535) ? 65535 : ((lm < 0) ? 0 : lm));
				col->nf = (uint16_t)((nfv > 65535) ? 65535 : ((nfv < 0) ? 0 : nfv));
			}
			col->mag_min = col_mmin;
			col->on_cnt = col_on;
			col->off_cnt = col_off;
			col->real_cnt = col_real;
			col->hops = col_hops;
			scope_pos = (uint16_t)((scope_pos + 1) % SCOPE_RING_SIZE);
			scope_total = scope_total + 1;      // volatile への ++ は非推奨
			taskEXIT_CRITICAL(&dsp_mux);
			col_mn = 32767;
			col_mx = -32768;
			col_m1 = 0;
			col_m2 = 0;
			col_mmin = 65535;
			col_gate_cnt = 0;
			col_on = col_off = col_real = 0;
			col_hops = 0;
			// 次列の周期をWPMから更新 (20WPM以下=2.0hop、40WPM=1.0hop)
			{
				uint16_t w = decoder_wpm();
				if (w == 0) w = 20;
				uint32_t q = (120UL << 8) / w;
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
				Serial.printf("[dsp] blk/s=%u mag avg=%d max=%d side=%d near=%d jit=%d smax=%d limit=%d nf=%d gate=%d hz=%u\n",
				              (unsigned)diag_blocks,
				              (int)(diag_mag_sum / n), (int)diag_mag_max,
				              (int)(diag_side_sum / n),
				              (int)(diag_near_sum / n),
				              (int)(diag_jit_sum / n),
				              (int)(diag_smax_sum / n),
				              (int)decoder_maglimit(), (int)decoder_noise_floor(),
				              (int)decoder_gate(),
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
	} while (0);
}

static void dsp_task(void *arg)
{
	dsp_alive = 1;
	for (;;) {
		dsp_step();
	}
}

//	一時停止: タスクを待機させてから ADC DMA を止める。
//	ADC continuous DMA を動かしたまま WiFi を起動すると割り込み
//	ウォッチドッグ (TG1WDT) でリセットされるため、WiFi を使う間は
//	必ず DMA を止める。再開時は ADC を初期化し直す。
void dsp_set_paused(uint8_t paused)
{
	if (paused) {
		if (dsp_paused) return;
		dsp_paused = 1;
		for (int i = 0; i < 50 && dsp_alive && !dsp_idle; i++) {
			delay(10);                  // 進行中の audio_read (最大7.4ms) を待つ
		}
		audio_stop();
		Serial.printf("[dsp] paused (idle=%d) heap=%u\n", (int)dsp_idle,
		              (unsigned)ESP.getFreeHeap());
	} else {
		if (!dsp_paused) return;
		audio_init();
		dsp_paused = 0;
		Serial.printf("[dsp] resumed heap=%u\n", (unsigned)ESP.getFreeHeap());
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
