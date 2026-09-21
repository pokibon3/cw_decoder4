//
//	タッチパネルの校正 (touchcal.h 参照)
//
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "touchcal.h"

#define NVS_NS  "cwdec"
#define NVS_KEY "touchcal"
#define PARAM_N 8
#define PARAM_BYTES (PARAM_N * sizeof(uint16_t))

#define C_BG      lgfx::color565(14, 17, 22)
#define C_TITLEBG lgfx::color565(26, 34, 46)
#define C_TITLETX lgfx::color565(200, 212, 226)
#define C_LABEL   lgfx::color565(120, 140, 165)
#define C_VALUE   lgfx::color565(235, 240, 246)
#define C_BTN_BG  lgfx::color565(26, 34, 46)
#define C_BTN_BD  lgfx::color565(70, 92, 120)
#define C_BTN_TX  lgfx::color565(190, 206, 226)
#define C_OK_BG   lgfx::color565(24, 54, 42)
#define C_OK_BD   lgfx::color565(90, 200, 140)
#define C_MARK    lgfx::color565(90, 200, 140)

#define TITLE_H 36
#define BTN_H 38
#define BTN_Y 190

// 確認画面 (2 ボタン)
#define BTN_W 130
#define BTN_L_X 20
#define BTN_R_X 170

// 開始画面 (3 ボタン: キャンセル / 既定に戻す / 開始)
#define BTN3_W 100
#define BTN3_X(i) (6 + (i) * 104)

static bool hit(int tx, int ty, int x, int y, int w, int h)
{
	return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

static void draw_button(LGFX *lcd, int x, int y, int w, int h, const char *label,
                        uint16_t bg, uint16_t bd)
{
	lcd->fillRoundRect(x, y, w, h, 6, bg);
	lcd->drawRoundRect(x, y, w, h, 6, bd);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_BTN_TX, bg);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString(label, x + w / 2, y + h / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void draw_title(LGFX *lcd, const char *title)
{
	lcd->fillScreen(C_BG);
	lcd->fillRect(0, 0, 320, TITLE_H, C_TITLEBG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_TITLETX, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_left);
	lcd->drawString(title, 8, TITLE_H / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void wait_release(LGFX *lcd)
{
	int32_t tx, ty;
	while (lcd->getTouch(&tx, &ty)) {
		delay(10);
	}
}

//	現在の設定 (= lgfx_config.h の x_min/x_max/y_min/y_max) から
//	Panel_Device::touchCalibrate() と同じ並びの校正値を作る
static void params_default(LGFX *lcd, uint16_t *p)
{
	if (lcd->touch() == nullptr) {
		memset(p, 0, PARAM_BYTES);
		return;
	}
	auto cfg = lcd->touch()->config();
	p[0] = cfg.x_min;  p[1] = cfg.y_min;
	p[2] = cfg.x_min;  p[3] = cfg.y_max;
	p[4] = cfg.x_max;  p[5] = cfg.y_min;
	p[6] = cfg.x_max;  p[7] = cfg.y_max;
}

static bool params_read(uint16_t *p)
{
	Preferences prefs;
	if (!prefs.begin(NVS_NS, false)) {
		return false;
	}
	bool ok = false;
	if (prefs.isKey(NVS_KEY)) {
		ok = (prefs.getBytes(NVS_KEY, p, PARAM_BYTES) == PARAM_BYTES);
	}
	prefs.end();
	return ok;
}

static bool params_write(const uint16_t *p)
{
	Preferences prefs;
	if (!prefs.begin(NVS_NS, false)) {
		Serial.println("[touch] NVS open failed");
		return false;
	}
	// putBytes = nvs_set_blob + nvs_commit。0 が返ったら書けていない
	size_t n = prefs.putBytes(NVS_KEY, p, PARAM_BYTES);
	prefs.end();
	if (n != PARAM_BYTES) {
		Serial.printf("[touch] NVS write failed (%u bytes)\n", (unsigned)n);
		return false;
	}
	return true;
}

//	4隅の生値が潰れていると setCalibrate の行列が解けず、座標が飛んで
//	どこを触っても反応しなくなる。そうなった値は読み込まない
#define RAW_SPAN_MIN 200

static bool params_sane(const uint16_t *p)
{
	uint16_t xmin = p[0], xmax = p[0], ymin = p[1], ymax = p[1];
	for (int i = 0; i < 4; i++) {
		uint16_t x = p[i * 2], y = p[i * 2 + 1];
		if (x < xmin) xmin = x;
		if (x > xmax) xmax = x;
		if (y < ymin) ymin = y;
		if (y > ymax) ymax = y;
	}
	return ((xmax - xmin) >= RAW_SPAN_MIN && (ymax - ymin) >= RAW_SPAN_MIN);
}

bool touchcal_saved(void)
{
	uint16_t p[PARAM_N];
	return params_read(p);
}

bool touchcal_load(LGFX *lcd)
{
	uint16_t p[PARAM_N];
	if (!params_read(p)) {
		return false;
	}
	if (!params_sane(p)) {
		Serial.println("[touch] stored calibration looks broken - ignored");
		return false;
	}
	lcd->setTouchCalibrate(p);
	Serial.printf("[touch] calibration loaded %u,%u %u,%u %u,%u %u,%u\n",
	              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	return true;
}

void touchcal_reset(LGFX *lcd)
{
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		prefs.remove(NVS_KEY);
		prefs.end();
	}
	uint16_t p[PARAM_N];
	params_default(lcd, p);
	lcd->setTouchCalibrate(p);
}

//==================================================================
//	四隅より内側の十字を4点タップして採点する
//
//	LovyanGFX の calibrateTouch() は画面のいちばん端 (0,0)〜(W-1,H-1) に
//	印を出すが、抵抗膜パネルの端はベゼルに近く狙いにくいうえ直線性も悪い。
//	そこで縦横 1/8 だけ内側に十字を出して測り、そこから四隅の生値へ外挿する。
//	生値は画面位置の一次式なので、内側の長方形4点があれば四隅は復元できる。
//
//	setCalibrate() に渡す並びは LovyanGFX の流儀に合わせる:
//	  index i -> x = (i>>1)&1 ? 右端 : 左端 / y = i&1 ? 下端 : 上端
//	で、座標系はタッチ素子の素の向き (回転オフセットを打ち消した回転)。
//==================================================================
#define INSET_DIV 8             // 画面の 1/8 だけ内側
#define SAMPLE_TIMEOUT_MS 30000 // この間タップが無ければ中止
#define RAWERR 20               // 連続2回の生値がこれ以内なら「静止」とみなす
#define CROSS_R 18

static void draw_cross(LGFX *lcd, int x, int y, uint16_t color)
{
	lcd->drawCircle(x, y, CROSS_R, color);
	lcd->drawCircle(x, y, 2, color);
	lcd->drawFastHLine(x - CROSS_R - 5, y, CROSS_R * 2 + 11, color);
	lcd->drawFastVLine(x, y - CROSS_R - 5, CROSS_R * 2 + 11, color);
}

//	1点ぶんの生値を拾う。静止した2回読みを8組平均する (LovyanGFX と同じ流儀)
static bool sample_point(LGFX *lcd, int32_t *rx, int32_t *ry)
{
	const uint32_t t0 = millis();
	int32_t sx = 0, sy = 0;
	lgfx::touch_point_t tp, tp2;

	for (int j = 0; j < 8; j++) {
		for (;;) {
			if ((millis() - t0) >= SAMPLE_TIMEOUT_MS) {
				return false;
			}
			if (!lcd->getTouchRaw(&tp, 1)) {
				delay(2);
				continue;
			}
			delay(10);
			if (!lcd->getTouchRaw(&tp2, 1)) {
				continue;
			}
			if (abs((int)tp.x - (int)tp2.x) > RAWERR ||
			    abs((int)tp.y - (int)tp2.y) > RAWERR) {
				continue;
			}
			break;
		}
		sx += (int32_t)tp.x + (int32_t)tp2.x;
		sy += (int32_t)tp.y + (int32_t)tp2.y;
	}
	*rx = sx >> 4;
	*ry = sy >> 4;
	return true;
}

static uint16_t clamp_raw(float v)
{
	if (v < 0.0f) return 0;
	if (v > 4095.0f) return 4095;
	return (uint16_t)(v + 0.5f);
}

//	内側4点 (s[]) から四隅の生値を外挿する。生値 = a*X + b*Y + c の一次式
static void extrapolate(const int32_t *s, int xl, int xr, int yt, int yb,
                        int w, int h, uint16_t *out, int stride)
{
	const float a = (float)((s[2] + s[3]) - (s[0] + s[1])) / (2.0f * (xr - xl));
	const float b = (float)((s[1] + s[3]) - (s[0] + s[2])) / (2.0f * (yb - yt));
	const float mean = (float)(s[0] + s[1] + s[2] + s[3]) / 4.0f;
	const float c = mean - a * ((xl + xr) * 0.5f) - b * ((yt + yb) * 0.5f);

	for (int i = 0; i < 4; i++) {
		const float x = ((i >> 1) & 1) ? (float)(w - 1) : 0.0f;
		const float y = (i & 1) ? (float)(h - 1) : 0.0f;
		out[i * 2 + stride] = clamp_raw(a * x + b * y + c);
	}
}

//	校正の本体。成功したら p[8] に四隅の生値が入る
static bool sample_corners(LGFX *lcd, uint16_t *p)
{
	// 案内は今の向き (横) で書いておく。回転を変えても画素はそのまま残る
	lcd->fillScreen(C_BG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_VALUE, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString("十字の中心を順にタップ", 160, 108);
	lcd->setTextColor(C_LABEL, C_BG);
	lcd->drawString("4点とも押すと確認画面になります", 160, 132);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	// タッチ素子の素の向きへ戻す (LovyanGFX の calibrate_touch と同じ式)
	const uint8_t p_off = lcd->panel()->config().offset_rotation;
	const uint8_t t_off = lcd->touch()->config().offset_rotation;
	const uint8_t rot_save = lcd->getRotation();
	lcd->setRotation((uint8_t)(((t_off ^ p_off) & 4) |
	                           ((uint8_t)(0 - (t_off + p_off)) & 3)));

	const int w = lcd->width(), h = lcd->height();
	const int xl = w / INSET_DIV, xr = w - 1 - w / INSET_DIV;
	const int yt = h / INSET_DIV, yb = h - 1 - h / INSET_DIV;

	int32_t sx[4], sy[4];
	bool ok = true;
	for (int i = 0; i < 4 && ok; i++) {
		const int x = ((i >> 1) & 1) ? xr : xl;
		const int y = (i & 1) ? yb : yt;
		draw_cross(lcd, x, y, C_VALUE);
		ok = sample_point(lcd, &sx[i], &sy[i]);
		draw_cross(lcd, x, y, C_BG);            // 消す (案内文は残る)
		if (ok) {
			lgfx::touch_point_t tp;
			while (lcd->getTouchRaw(&tp, 1)) {  // 離すまで待つ
				delay(2);
			}
		}
	}

	if (ok) {
		extrapolate(sx, xl, xr, yt, yb, w, h, p, 0);
		extrapolate(sy, xl, xr, yt, yb, w, h, p, 1);
	}
	lcd->setRotation(rot_save);
	return ok;
}

#define START_CANCEL 0
#define START_RUN    1
#define START_RESET  2

//	開始画面。校正を始めるか / 既定値へ戻すか / やめるか
static int ask_start(LGFX *lcd)
{
	draw_title(lcd, "タッチ調整");
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_VALUE, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	lcd->drawString("画面の四隅に印が出ます", 160, 58);
	lcd->drawString("印の中心を順にタップしてください", 160, 82);
	lcd->setTextColor(C_LABEL, C_BG);
	lcd->drawString("先の細いもの (爪の先など) が正確です", 160, 116);
	lcd->drawString(touchcal_saved() ? "現在: 調整済み" : "現在: 既定値", 160, 140);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	draw_button(lcd, BTN3_X(0), BTN_Y, BTN3_W, BTN_H, "キャンセル", C_BTN_BG, C_BTN_BD);
	draw_button(lcd, BTN3_X(1), BTN_Y, BTN3_W, BTN_H, "既定に戻す", C_BTN_BG, C_BTN_BD);
	draw_button(lcd, BTN3_X(2), BTN_Y, BTN3_W, BTN_H, "開始", C_OK_BG, C_OK_BD);

	wait_release(lcd);
	int32_t tx, ty;
	for (;;) {
		if (lcd->getTouch(&tx, &ty)) {
			int idx = -1;
			for (int i = 0; i < 3; i++) {
				if (hit(tx, ty, BTN3_X(i), BTN_Y, BTN3_W, BTN_H)) {
					idx = i;
					break;
				}
			}
			wait_release(lcd);
			if (idx == 2) return START_RUN;
			if (idx == 1) return START_RESET;
			if (idx == 0) return START_CANCEL;
		}
		delay(10);
	}
}

//	校正直後の確認。
//	単に [保存] を置くだけだと、180°ずれた補正のまま適当に触った指が
//	たまたま [保存] に当たって「ズレたまま保存」できてしまう。
//	そこで画面の3点 (左上 / 右上 / 下中央) を順に当ててもらい、全部当たって
//	はじめて [保存] を有効にする。3点は 180°回した位置が互いに重ならないよう
//	選んであるので、上下逆のまま通ることはない。
//	当たらないまま放置されたら「押せない = ズレたまま」とみなして破棄する。
#define VERIFY_TIMEOUT_MS 45000

#define KEEP_SAVE   1   // [保存]
#define KEEP_RETRY  0   // [やり直す]
#define KEEP_ABORT -1   // 3点に当てられない = SETUP へ戻る

#define TGT_N 3
#define TGT_R 22        // 当たり半径
#define TGT_DRAW_R 16   // 印の大きさ

static const struct { int16_t x, y; } targets[TGT_N] = {
	{  52,  90 },
	{ 268,  90 },
	{ 160, 158 },
};

static void draw_target(LGFX *lcd, int i, bool done)
{
	const uint16_t c = done ? C_MARK : C_VALUE;
	const int x = targets[i].x, y = targets[i].y;
	lcd->drawCircle(x, y, TGT_DRAW_R, c);
	lcd->drawFastHLine(x - TGT_DRAW_R - 4, y, TGT_DRAW_R * 2 + 9, c);
	lcd->drawFastVLine(x, y - TGT_DRAW_R - 4, TGT_DRAW_R * 2 + 9, c);
	if (done) {
		lcd->fillCircle(x, y, 5, C_MARK);
	}
}

static bool near_target(int i, int32_t tx, int32_t ty)
{
	int dx = (int)tx - targets[i].x;
	int dy = (int)ty - targets[i].y;
	return (dx * dx + dy * dy) <= (TGT_R * TGT_R);
}

static void draw_progress(LGFX *lcd, int done, uint32_t left_sec)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "印をタップ  %d/%d   (%u秒)",
	         done, TGT_N, (unsigned)left_sec);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_LABEL, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_right);
	lcd->fillRect(140, 2, 176, TITLE_H - 4, C_TITLEBG);
	lcd->drawString(buf, 314, TITLE_H / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static int ask_keep(LGFX *lcd)
{
	draw_title(lcd, "調整の確認");
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_LABEL, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	lcd->drawString("印の中心を順にタップしてください", 160, 44);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	int done = 0;
	draw_target(lcd, 0, false);
	draw_button(lcd, BTN_L_X, BTN_Y, BTN_W, BTN_H, "やり直す", C_BTN_BG, C_BTN_BD);
	draw_button(lcd, BTN_R_X, BTN_Y, BTN_W, BTN_H, "保存", C_BG, C_BTN_BD);

	wait_release(lcd);

	uint32_t t0 = millis();         // 進んだら測り直す
	uint32_t left_shown = 0xFFFFFFFF;
	int32_t tx, ty;
	for (;;) {
		uint32_t elapsed = millis() - t0;
		if (elapsed >= VERIFY_TIMEOUT_MS) {
			return KEEP_ABORT;
		}
		uint32_t left = (VERIFY_TIMEOUT_MS - elapsed) / 1000;
		if (left != left_shown) {
			left_shown = left;
			draw_progress(lcd, done, left);
		}

		if (!lcd->getTouch(&tx, &ty)) {
			delay(10);
			continue;
		}
		if (hit(tx, ty, BTN_L_X, BTN_Y, BTN_W, BTN_H)) {
			wait_release(lcd);
			return KEEP_RETRY;
		}
		if (done >= TGT_N && hit(tx, ty, BTN_R_X, BTN_Y, BTN_W, BTN_H)) {
			wait_release(lcd);
			return KEEP_SAVE;
		}
		if (done < TGT_N && near_target(done, tx, ty)) {
			draw_target(lcd, done, true);
			done++;
			if (done < TGT_N) {
				draw_target(lcd, done, false);
			} else {        // 3点とも当たった: [保存] を押せるようにする
				draw_button(lcd, BTN_R_X, BTN_Y, BTN_W, BTN_H, "保存",
				            C_OK_BG, C_OK_BD);
			}
			t0 = millis();
			left_shown = 0xFFFFFFFF;
			wait_release(lcd);
			continue;
		}
		// 外したところにも印を出す (どこへズレているか見えるように)。
		// ボタンの上だけは汚さない
		if (!hit(tx, ty, BTN_L_X, BTN_Y, BTN_W, BTN_H) &&
		    !hit(tx, ty, BTN_R_X, BTN_Y, BTN_W, BTN_H) &&
		    ty >= TITLE_H) {
			lcd->fillCircle(tx, ty, 2, C_BTN_BD);
		}
		delay(10);
	}
}

void touchcal_run(LGFX *lcd)
{
	uint16_t before[PARAM_N];
	if (!params_read(before)) {
		params_default(lcd, before);
	}

	for (;;) {
		int act = ask_start(lcd);
		if (act == START_CANCEL) {
			return;
		}
		if (act == START_RESET) {
			touchcal_reset(lcd);
			params_default(lcd, before);
			continue;
		}

		uint16_t p[PARAM_N];
		if (!sample_corners(lcd, p)) {
			continue;               // 時間切れ: 何も変えずに開始画面へ
		}
		Serial.printf("[touch] raw corners %u,%u %u,%u %u,%u %u,%u\n",
		              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		if (!params_sane(p)) {      // 潰れた値は適用も保存もしない
			Serial.println("[touch] measurement looks broken - discarded");
			continue;
		}
		lcd->setTouchCalibrate(p);  // 確認画面はこの新しい補正で動かす

		int keep = ask_keep(lcd);
		if (keep == KEEP_SAVE) {
			bool ok = params_write(p);
			Serial.printf("[touch] calibration %s %u,%u %u,%u %u,%u %u,%u\n",
			              ok ? "saved" : "NOT SAVED",
			              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			draw_title(lcd, "タッチ調整");
			lcd->setFont(&fonts::lgfxJapanGothicP_16);
			lcd->setTextColor(ok ? C_VALUE : C_TITLETX, C_BG);
			lcd->setTextDatum(lgfx::textdatum_t::middle_center);
			lcd->drawString(ok ? "保存しました (NVS)" : "保存できませんでした",
			                160, 120);
			lcd->setTextDatum(lgfx::textdatum_t::top_left);
			delay(1200);
			return;
		}
		lcd->setTouchCalibrate(before);         // 破棄: 元の設定へ戻す
		if (keep == KEEP_ABORT) {
			return;                 // 元に戻した状態で SETUP へ
		}
	}
}
