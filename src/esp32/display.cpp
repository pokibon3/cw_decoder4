//
//	画面構成 (320x240 横):
//	  y   0..17  ステータス行 (モード / WPM / トーン / ピーク周波数 / 信号)
//	  y  20..163 デコード文字エリア 15列 x 6行 (24x24 全角フォント、ピッチ21px)
//	  y 166..239 左: FFTスペクトラム / 右: オシロスコープ
//	オシロは生波形(min/maxバンド)・トーンエンベロープ・キー判定を
//	同一時間軸(1列=6ms)で色分け重畳する。
//
#include <Arduino.h>
#include <string.h>
#include "lgfx_config.h"
#include "display.h"
#include "decoder.h"
#include "decode.h"
#include "dsp.h"

#define STATUS_H 18
#define TEXT_TOP 20
#define TEXT_X0 0
#define TEXT_COLS 16
#define TEXT_ROWS 6
// 文字ピッチはフォント幅(24px)より詰める: 全角グリフの左右余白ぶんを
// 相殺して間延びを防ぐ。グリフはピッチ中央に透過描画する。
#define CELL_W 20
#define CELL_H 24
#define GLYPH_W 24
#define PANEL_TOP 166
#define PANEL_H 74
#define PANEL_W 160
#define SCOPE_COLS 150

// カラーパレット
#define C_STATUS_BG lgfx::color565(8, 20, 45)
#define C_STATUS_TX lgfx::color565(120, 210, 255)
#define C_WPM       lgfx::color565(250, 199, 117)
#define C_BADGE_BG  lgfx::color565(250, 199, 117)
#define C_BADGE_TX  lgfx::color565(30, 25, 5)
#define C_SEP       lgfx::color565(60, 90, 130)
#define C_TEXT      lgfx::color565(225, 230, 220)
#define C_TEXT_NEW  lgfx::color565(173, 255, 47)
#define C_CURSOR    lgfx::color565(250, 199, 117)
#define C_PANEL_BG  lgfx::color565(2, 8, 16)
#define C_FRAME     lgfx::color565(40, 60, 85)
#define C_LABEL     lgfx::color565(100, 125, 155)
#define C_EQ_FILL   lgfx::color565(8, 65, 50)
#define C_EQ_LINE   lgfx::color565(60, 230, 160)
#define C_EQ_PEAK   lgfx::color565(210, 215, 225)
#define C_MARKER    lgfx::color565(50, 75, 130)
#define C_BAND      lgfx::color565(10, 24, 42)
#define C_TONE_BAND lgfx::color565(70, 52, 16)
#define C_RAW       lgfx::color565(0, 130, 175)
#define C_ENV       lgfx::color565(250, 199, 117)
#define C_GATE      lgfx::color565(90, 220, 120)
#define C_GATE_OFF  lgfx::color565(35, 55, 45)

static LGFX lcd;
static LGFX_Sprite fft_spr(&lcd);
static LGFX_Sprite scope_spr(&lcd);
static LGFX_Sprite status_spr(&lcd);

static QueueHandle_t char_queue;

static uint16_t grid[TEXT_ROWS][TEXT_COLS];
static uint8_t cur_row = 0;
static uint8_t cur_col = 0;
static uint8_t last_valid = 0;
static uint8_t last_row = 0;
static uint8_t last_col = 0;

//==================================================================
//	JIS X 0201 カナ → 全角カタカナ Unicode
//==================================================================
static const uint16_t kana_cp[0x3F] = {
	0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2,          // 。「」、・ヲ
	0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,                  // ァィゥェォ
	0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,                  // ャュョッー
	0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA,                  // アイウエオ
	0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,                  // カキクケコ
	0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,                  // サシスセソ
	0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,                  // タチツテト
	0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE,                  // ナニヌネノ
	0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,                  // ハヒフヘホ
	0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2,                  // マミムメモ
	0x30E4, 0x30E6, 0x30E8,                                  // ヤユヨ
	0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED,                  // ラリルレロ
	0x30EF, 0x30F3, 0x309B, 0x309C                           // ワン゛゜
};

// 濁点が付けられる清音か (カ〜ト行 + ハ行 + ウ)
static uint8_t can_dakuten(uint16_t cp)
{
	static const uint16_t tbl[] = {
		0x30A6,                                              // ウ→ヴ
		0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,              // カ行
		0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,              // サ行
		0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,              // タ行
		0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB               // ハ行
	};
	for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (tbl[i] == cp) return 1;
	}
	return 0;
}

static uint8_t can_handakuten(uint16_t cp)
{
	return (cp >= 0x30CF && cp <= 0x30DB &&
	        (cp == 0x30CF || cp == 0x30D2 || cp == 0x30D5 || cp == 0x30D8 || cp == 0x30DB));
}

static int utf8_encode(uint16_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		out[1] = '\0';
		return 1;
	}
	out[0] = (char)(0xE0 | (cp >> 12));
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(0x80 | (cp & 0x3F));
	out[3] = '\0';
	return 3;
}

//==================================================================
//	デコード文字エリア
//==================================================================
static void draw_cell(uint8_t r, uint8_t c, uint16_t color)
{
	int x = TEXT_X0 + c * CELL_W;
	int y = TEXT_TOP + r * CELL_H;
	uint16_t cp = grid[r][c];

	lcd.fillRect(x, y, CELL_W, CELL_H, TFT_BLACK);
	if (cp == ' ' || cp == 0) return;

	// 英数字・記号は全角形 (U+FF01〜) に変換し、全て24x24全角フォントで表示
	if (cp >= 0x21 && cp <= 0x7E) {
		cp = (uint16_t)(0xFF01 + cp - 0x21);
	}
	char buf[4];
	utf8_encode(cp, buf);
	lcd.setFont(&fonts::lgfxJapanGothic_24);
	lcd.setTextColor(color);   // 透過描画 (背景は fillRect 済み)
	lcd.setCursor(x + (CELL_W - GLYPH_W) / 2, y);
	lcd.print(buf);
}

static void draw_cursor(uint8_t r, uint8_t c, uint8_t on)
{
	int x = TEXT_X0 + c * CELL_W;
	int y = TEXT_TOP + r * CELL_H + CELL_H - 2;
	lcd.fillRect(x + 2, y, CELL_W - 4, 2, on ? C_CURSOR : TFT_BLACK);
}

static void redraw_text_area(void)
{
	lcd.startWrite();
	for (uint8_t r = 0; r < TEXT_ROWS; r++) {
		for (uint8_t c = 0; c < TEXT_COLS; c++) {
			uint16_t color = (last_valid && r == last_row && c == last_col)
				? C_TEXT_NEW : C_TEXT;
			draw_cell(r, c, color);
		}
	}
	lcd.endWrite();
}

static void text_newline(void)
{
	draw_cursor(cur_row, cur_col, 0);
	cur_col = 0;
	if (cur_row < TEXT_ROWS - 1) {
		cur_row++;
	} else {
		memmove(grid[0], grid[1], sizeof(grid[0]) * (TEXT_ROWS - 1));
		for (uint8_t c = 0; c < TEXT_COLS; c++) {
			grid[TEXT_ROWS - 1][c] = ' ';
		}
		if (last_valid && last_row > 0) {
			last_row--;
		} else {
			last_valid = 0;
		}
		redraw_text_area();
	}
}

static void text_putcp(uint16_t cp)
{
	// 濁点/半濁点は直前の文字と合成 (カ+゛→ガ)
	if (last_valid && (cp == 0x309B || cp == 0x309C)) {
		uint16_t prev = grid[last_row][last_col];
		uint16_t comb = 0;
		if (cp == 0x309B && can_dakuten(prev)) {
			comb = (prev == 0x30A6) ? 0x30F4 : (uint16_t)(prev + 1);
		} else if (cp == 0x309C && can_handakuten(prev)) {
			comb = (uint16_t)(prev + 2);
		}
		if (comb != 0) {
			grid[last_row][last_col] = comb;
			draw_cell(last_row, last_col, C_TEXT_NEW);
			return;
		}
	}

	if (cur_col >= TEXT_COLS) {
		text_newline();
	}
	if (last_valid) {
		draw_cell(last_row, last_col, C_TEXT);
	}
	grid[cur_row][cur_col] = cp;
	draw_cell(cur_row, cur_col, C_TEXT_NEW);
	last_valid = 1;
	last_row = cur_row;
	last_col = cur_col;
	cur_col++;
	if (cur_col < TEXT_COLS) {
		draw_cursor(cur_row, cur_col, 1);
	}
}

static void text_putchar(uint8_t ch)
{
	uint16_t cp;
	if (ch == 5) {
		cp = 0x300C;              // ホレ (和文開始) → 「
	} else if (ch == 6) {
		cp = 0x300D;              // ラタ (和文終了) → 」
	} else if (ch >= 0xA1 && ch <= 0xDF) {
		cp = kana_cp[ch - 0xA1];
	} else if (ch < 0x80) {
		cp = ch;
	} else {
		cp = '*';
	}

	if (cp == ' ') {
		if (last_valid) {
			draw_cell(last_row, last_col, C_TEXT);
			last_valid = 0;
		}
		if (cur_col >= TEXT_COLS) {
			text_newline();
			return;                // 行頭のスペースは捨てる
		}
		draw_cursor(cur_row, cur_col, 0);
		grid[cur_row][cur_col] = ' ';
		cur_col++;
		if (cur_col < TEXT_COLS) {
			draw_cursor(cur_row, cur_col, 1);
		}
		return;
	}
	text_putcp(cp);
}

//==================================================================
//	ステータス行
//==================================================================
static void draw_status(void)
{
	static uint16_t s_wpm = 0xFFFF;
	static uint8_t s_mode = 0xFF;
	static uint8_t s_tone = 0xFF;
	static uint16_t s_thz = 0xFFFF;
	static uint8_t s_gate = 0xFF;
	static uint16_t s_peak = 0xFFFF;
	static uint32_t s_peak_ms = 0;

	uint16_t wpm = decoder_wpm();
	uint8_t mode = decoder_mode();
	uint8_t tone = dsp_tone_index();
	uint16_t thz = dsp_tone_hz();
	uint8_t gate = decoder_gate();
	uint16_t peak = dsp_peak_hz();

	// ピーク周波数の更新は250ms毎に間引く
	uint32_t now = millis();
	if ((now - s_peak_ms) < 250 && peak != 0 && s_peak != 0xFFFF) {
		peak = s_peak;
	}

	if (wpm == s_wpm && mode == s_mode && tone == s_tone && thz == s_thz &&
	    gate == s_gate && peak == s_peak) {
		return;
	}
	if (peak != s_peak) s_peak_ms = now;
	s_wpm = wpm; s_mode = mode; s_tone = tone; s_thz = thz;
	s_gate = gate; s_peak = peak;

	char buf[24];
	status_spr.fillSprite(C_STATUS_BG);
	status_spr.setFont(&fonts::AsciiFont8x16);

	// モードバッジ (タッチボタン)
	status_spr.fillRoundRect(2, 1, 30, 16, 3, C_BADGE_BG);
	status_spr.setTextColor(C_BADGE_TX);
	status_spr.setCursor(9, 1);
	status_spr.print((mode == MODE_US) ? "US" : "JP");

	// WPM
	status_spr.setTextColor(C_WPM);
	status_spr.setCursor(40, 1);
	snprintf(buf, sizeof(buf), "%2dWPM", wpm);
	status_spr.print(buf);

	// 選択トーン周波数 (タッチボタン)。AUTO時は追従周波数を表示
	status_spr.drawRoundRect(98, 0, 104, 18, 4, C_SEP);
	if (dsp_tone_is_auto()) {
		status_spr.setTextColor(C_GATE);
		snprintf(buf, sizeof(buf), "AUTO %4dHz", thz);
	} else {
		status_spr.setTextColor(C_STATUS_TX);
		snprintf(buf, sizeof(buf), "TONE %4dHz", thz);
	}
	status_spr.setCursor(106, 1);
	status_spr.print(buf);

	// 実測ピーク周波数
	status_spr.setTextColor(C_GATE);
	status_spr.setCursor(210, 1);
	if (peak != 0) {
		snprintf(buf, sizeof(buf), "PK %4dHz", peak);
	} else {
		snprintf(buf, sizeof(buf), "PK ----");
	}
	status_spr.print(buf);

	// 信号インジケータ
	status_spr.fillCircle(310, 9, 5, gate ? C_GATE : C_GATE_OFF);

	status_spr.pushSprite(0, 0);
}

//==================================================================
//	FFTスペクトラムパネル (左下)
//	表示帯域 約300〜1200Hz、1点 = 1bin (31.25Hz)、
//	ライン+塗りのスペクトラム表示 (縦はピクセル解像度)
//==================================================================
#define EQ_BAR_COUNT 29          // 1点 = 1bin (31.25Hz)、bin10〜38
#define EQ_BIN_START 10          // 312.5Hz
#define EQ_BAR_PITCH 5           // 5px/bin
#define EQ_X0 6
#define EQ_BASE_Y 62
#define EQ_PLOT_H 50
#define EQ_F_MIN 312.5f          // bin10 の中心周波数
#define EQ_HZ_PER_PX (31.25f / (float)EQ_BAR_PITCH)

static int eq_x_of_hz(float f)
{
	return EQ_X0 + (int)((f - EQ_F_MIN) / EQ_HZ_PER_PX + 0.5f);
}

static void draw_fft_panel(void)
{
	static float bar[EQ_BAR_COUNT] = { 0 };
	static float peak_px[EQ_BAR_COUNT] = { 0 };
	static uint8_t peak_ttl[EQ_BAR_COUNT] = { 0 };
	static float disp_max = 8000.0f;
	uint16_t spec[DSP_SPEC_BINS + 1];

	dsp_get_spectrum(spec);

	const int plot_h = EQ_PLOT_H;
	const int plot_top = EQ_BASE_Y - plot_h;

	// AGC: 表示帯域のフレーム最大値にゆっくり追従
	float fmax = 0.0f;
	for (int b = 0; b < EQ_BAR_COUNT; b++) {
		float m = (float)spec[EQ_BIN_START + b];
		if (m > fmax) fmax = m;
	}
	disp_max += (fmax * 1.15f - disp_max) * 0.05f;
	if (disp_max < 8000.0f) disp_max = 8000.0f;

	fft_spr.fillSprite(C_PANEL_BG);
	fft_spr.drawRect(0, 0, PANEL_W, PANEL_H, C_FRAME);

	// TONE選択レンジ(600〜1000Hz)のガイド帯 + 境界線
	{
		int x_lo = eq_x_of_hz((float)dsp_tone_hz_at(0));
		int x_hi = eq_x_of_hz((float)dsp_tone_hz_at(DSP_TONE_COUNT - 1));
		fft_spr.fillRect(x_lo, plot_top - 2, x_hi - x_lo + 1, plot_h + 4, C_BAND);
		fft_spr.drawFastVLine(x_lo, plot_top - 2, plot_h + 4, C_MARKER);
		fft_spr.drawFastVLine(x_hi, plot_top - 2, plot_h + 4, C_MARKER);
	}

	// 選択中トーンの検出帯域 (Goertzel 1ビン幅 = 中心±41.7Hz) を帯で表示
	// AUTO時は追従先へスライドする
	{
		const float half_bw = (float)DSP_SAMPLE_RATE / (float)DSP_GATE_WIN * 0.5f;
		int xc = eq_x_of_hz((float)dsp_tone_hz());
		int hw = (int)(half_bw / EQ_HZ_PER_PX + 0.5f);
		fft_spr.fillRect(xc - hw, plot_top - 2, hw * 2 + 1, plot_h + 4, C_TONE_BAND);
		fft_spr.drawFastVLine(xc - hw, plot_top - 2, plot_h + 4, C_ENV);
		fft_spr.drawFastVLine(xc + hw, plot_top - 2, plot_h + 4, C_ENV);
	}

	// スペクトラム本体: 塗り + エンベロープライン + ピークホールド
	{
		int y_pt[EQ_BAR_COUNT];
		for (int b = 0; b < EQ_BAR_COUNT; b++) {
			float m = (float)spec[EQ_BIN_START + b];
			float t = m / disp_max * (float)plot_h;
			if (t > (float)plot_h) t = (float)plot_h;
			if (t >= bar[b]) {
				bar[b] += (t - bar[b]) * 0.7f;
			} else {
				bar[b] += (t - bar[b]) * 0.35f;
			}
			y_pt[b] = EQ_BASE_Y - (int)(bar[b] + 0.5f);

			if (bar[b] >= peak_px[b]) {
				peak_px[b] = bar[b];
				peak_ttl[b] = 25;
			} else if (peak_ttl[b] > 0) {
				peak_ttl[b]--;
			} else if (peak_px[b] > 0.0f) {
				peak_px[b] -= 1.0f;
			}
		}
		// 塗り (点間は線形補間)
		for (int b = 0; b < EQ_BAR_COUNT - 1; b++) {
			int x = EQ_X0 + b * EQ_BAR_PITCH;
			for (int dx = 0; dx < EQ_BAR_PITCH; dx++) {
				int yy = y_pt[b] + ((y_pt[b + 1] - y_pt[b]) * dx) / EQ_BAR_PITCH;
				if (yy < EQ_BASE_Y) {
					fft_spr.drawFastVLine(x + dx, yy, EQ_BASE_Y - yy, C_EQ_FILL);
				}
			}
		}
		// エンベロープライン
		for (int b = 0; b < EQ_BAR_COUNT - 1; b++) {
			int x = EQ_X0 + b * EQ_BAR_PITCH;
			fft_spr.drawLine(x, y_pt[b], x + EQ_BAR_PITCH, y_pt[b + 1], C_EQ_LINE);
		}
		// ピークホールド (bin毎の白マーカー)
		for (int b = 0; b < EQ_BAR_COUNT; b++) {
			if (peak_px[b] > 1.0f) {
				int x = EQ_X0 + b * EQ_BAR_PITCH - 1;
				if (x < EQ_X0) x = EQ_X0;
				fft_spr.drawFastHLine(x, EQ_BASE_Y - (int)(peak_px[b] + 0.5f), 3, C_EQ_PEAK);
			}
		}
	}

	// 選択中トーンの中心マーカー (下端の三角)
	{
		int xm = eq_x_of_hz((float)dsp_tone_hz());
		fft_spr.fillTriangle(xm - 3, EQ_BASE_Y + 9, xm + 3, EQ_BASE_Y + 9, xm, EQ_BASE_Y + 3, C_ENV);
	}

	// 周波数目盛
	fft_spr.setFont(&fonts::Font0);
	fft_spr.setTextColor(C_LABEL);
	fft_spr.setCursor(4, 3);
	fft_spr.print("FFT 0.3-1.2k");
	{
		int x6 = eq_x_of_hz(600.0f);
		int x10 = eq_x_of_hz(1000.0f);
		fft_spr.setCursor(x6 - 8, EQ_BASE_Y + 4);
		fft_spr.print("600");
		fft_spr.setCursor(x10 - 5, EQ_BASE_Y + 4);
		fft_spr.print("1k");
	}

	fft_spr.pushSprite(0, PANEL_TOP);
}

//==================================================================
//	オシロスコープパネル (右下)
//	生波形 min/max バンド(シアン) + エンベロープ(アンバー) +
//	キー判定(グリーン, 上端バー) を同一時間軸で重畳
//==================================================================
static void draw_scope_panel(void)
{
	static scope_col_t cols[SCOPE_COLS];
	static float raw_max = 100.0f;
	static float env_max = 500.0f;

	dsp_get_scope(cols, SCOPE_COLS);

	const int x0 = 4;
	const int mid_y = 42;
	const int half_h = 28;
	const int env_base = 70;
	const int env_h = 56;

	// AGC (生波形/エンベロープ別)
	float rmax = 0.0f, emax = 0.0f;
	for (int i = 0; i < SCOPE_COLS; i++) {
		float a = (float)((cols[i].mx > -cols[i].mn) ? cols[i].mx : -cols[i].mn);
		if (a > rmax) rmax = a;
		if ((float)cols[i].mag > emax) emax = (float)cols[i].mag;
	}
	raw_max += (rmax * 1.1f - raw_max) * 0.1f;
	if (raw_max < 60.0f) raw_max = 60.0f;
	env_max += (emax * 1.1f - env_max) * 0.1f;
	if (env_max < 400.0f) env_max = 400.0f;

	scope_spr.fillSprite(C_PANEL_BG);
	scope_spr.drawRect(0, 0, PANEL_W, PANEL_H, C_FRAME);
	scope_spr.drawFastHLine(x0, mid_y, SCOPE_COLS, lgfx::color565(20, 40, 55));

	int prev_ey = -1;
	for (int i = 0; i < SCOPE_COLS; i++) {
		int x = x0 + i;

		// 生波形 min/max バンド
		int y1 = mid_y - (int)((float)cols[i].mx / raw_max * (float)half_h);
		int y2 = mid_y - (int)((float)cols[i].mn / raw_max * (float)half_h);
		if (y1 < mid_y - half_h) y1 = mid_y - half_h;
		if (y2 > mid_y + half_h) y2 = mid_y + half_h;
		if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
		scope_spr.drawFastVLine(x, y1, y2 - y1 + 1, C_RAW);

		// エンベロープ (下端基準の折れ線)
		int eh = (int)((float)cols[i].mag / env_max * (float)env_h);
		if (eh > env_h) eh = env_h;
		int ey = env_base - eh;
		if (prev_ey >= 0) {
			scope_spr.drawLine(x - 1, prev_ey, x, ey, C_ENV);
		}
		prev_ey = ey;

		// キー判定バー
		if (cols[i].gate) {
			scope_spr.drawFastVLine(x, 12, 3, C_GATE);
		}
	}

	scope_spr.setFont(&fonts::Font0);
	scope_spr.setTextColor(C_LABEL);
	scope_spr.setCursor(4, 3);
	scope_spr.print("SCOPE 3.6s");
	scope_spr.setTextColor(C_GATE);
	scope_spr.setCursor(76, 3);
	scope_spr.print("KEY");
	scope_spr.setTextColor(C_ENV);
	scope_spr.setCursor(102, 3);
	scope_spr.print("ENV");
	scope_spr.setTextColor(C_RAW);
	scope_spr.setCursor(128, 3);
	scope_spr.print("RAW");

	scope_spr.pushSprite(PANEL_W, PANEL_TOP);
}

//==================================================================
//	公開API
//==================================================================
void display_init(void)
{
	lcd.init();
	lcd.setRotation(1);           // 320x240 横
	lcd.setColorDepth(16);
	lcd.fillScreen(TFT_BLACK);
	lcd.setBrightness(200);
	// 右端セルのグリフ(ピッチ20px < フォント幅24px)が自動折り返しで
	// 消えないようにする
	lcd.setTextWrap(false);

	status_spr.setColorDepth(16);
	status_spr.createSprite(320, STATUS_H);
	fft_spr.setColorDepth(16);
	fft_spr.createSprite(PANEL_W, PANEL_H);
	scope_spr.setColorDepth(16);
	scope_spr.createSprite(PANEL_W, PANEL_H);

	char_queue = xQueueCreate(128, sizeof(uint8_t));

	for (uint8_t r = 0; r < TEXT_ROWS; r++) {
		for (uint8_t c = 0; c < TEXT_COLS; c++) {
			grid[r][c] = ' ';
		}
	}
}

void display_splash(void)
{
	lcd.fillScreen(lgfx::color565(4, 10, 24));
	lcd.setTextDatum(lgfx::textdatum_t::middle_center);

	// タイトル (Orbitron: ネイティブ32px、拡大なし)
	lcd.setFont(&fonts::Orbitron_Light_32);
	lcd.setTextColor(C_STATUS_TX);
	lcd.drawString("CW DECODER", 160, 76);

	// モールス飾り: "CQ" (-.-. --.-)
	{
		const char *m = "-.-. --.-";
		int w = 0;
		for (const char *p = m; *p; p++) {
			w += (*p == '-') ? 19 : (*p == '.') ? 10 : 15;
		}
		int x = (320 - w) / 2;
		for (const char *p = m; *p; p++) {
			if (*p == '-') {
				lcd.fillRoundRect(x, 116, 14, 5, 2, C_WPM);
				x += 19;
			} else if (*p == '.') {
				lcd.fillRoundRect(x, 116, 5, 5, 2, C_WPM);
				x += 10;
			} else {
				x += 15;
			}
		}
	}

	lcd.drawFastHLine(40, 140, 240, C_SEP);

	lcd.setFont(&fonts::Orbitron_Light_24);
	lcd.setTextColor(C_EQ_LINE);
	lcd.drawString("ESP32", 160, 168);

	lcd.setFont(&fonts::Font2);
	lcd.setTextColor(C_LABEL);
	lcd.drawString("ST7789 / LovyanGFX  -  Version 2.0", 160, 202);

	lcd.setTextDatum(lgfx::textdatum_t::top_left);
	delay(1500);
	lcd.fillScreen(TFT_BLACK);
	lcd.drawFastHLine(0, STATUS_H, 320, C_SEP);
	lcd.drawFastHLine(0, PANEL_TOP - 2, 320, C_SEP);
	draw_cursor(0, 0, 1);
}

void display_enqueue(uint8_t ch)
{
	if (char_queue) {
		xQueueSend(char_queue, &ch, 0);
	}
}

//==================================================================
//	タッチ操作
//	- US/JPバッジ: モード切替
//	- TONE表示: トーン周波数を順送り (600→700→800→900→1000)
//	- FFTパネル内: タップ位置の周波数に最も近いトーンを直接選択
//==================================================================
#define TOUCH_DEBUG 0

static void poll_touch(void)
{
	static uint8_t touching = 0;
	static uint32_t last_act_ms = 0;

	int32_t x, y;
	uint8_t now = (lcd.getTouch(&x, &y) > 0);

#if TOUCH_DEBUG
	if (now) {
		static uint32_t dbg_ms = 0;
		lcd.fillCircle(x, y, 3, TFT_RED);
		if (millis() - dbg_ms > 200) {
			dbg_ms = millis();
			lgfx::touch_point_t tp;
			int raw_n = lcd.getTouchRaw(&tp, 1);
			Serial.printf("[diag] touch x=%d y=%d raw_n=%d raw=(%d,%d)\n",
			              (int)x, (int)y, raw_n, (int)tp.x, (int)tp.y);
		}
	}
#endif
	if (now && !touching) {
		uint32_t t = millis();
		if ((t - last_act_ms) >= 250) {
			if (y < TEXT_TOP + 8) {
				// ステータス行 (少し下までタップ許容)
				if (x < 70) {
					decoder_toggle_mode();
					last_act_ms = t;
				} else if (x >= 92 && x < 210) {
					dsp_set_tone((uint8_t)((dsp_tone_index() + 1) % (DSP_TONE_COUNT + 1)));
					last_act_ms = t;
				}
			} else if (y >= PANEL_TOP && x < PANEL_W) {
				// FFTパネル: タップ位置の周波数に最も近いトーンを選択
				float f = EQ_F_MIN + (float)(x - EQ_X0) * EQ_HZ_PER_PX;
				if (f >= 400.0f && f <= 1200.0f) {
					uint8_t best = 0;
					float best_d = 1e9f;
					for (uint8_t i = 0; i < DSP_TONE_COUNT; i++) {
						float d = fabsf(f - (float)dsp_tone_hz_at(i));
						if (d < best_d) {
							best_d = d;
							best = i;
						}
					}
					dsp_set_tone(best);
					last_act_ms = t;
				}
			}
		}
	}
	touching = now;
}

void display_update(void)
{
	poll_touch();

	uint8_t ch;
	int budget = 8;
	while (budget-- > 0 && xQueueReceive(char_queue, &ch, 0) == pdTRUE) {
		text_putchar(ch);
	}
	draw_status();
	draw_fft_panel();
	draw_scope_panel();
}
