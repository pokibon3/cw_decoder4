//
//	パタパタ時計の描画
//
//	配色は写真のパタパタ時計に合わせた黒カード + 白抜きの数字。
//
//	画面 (320x240 横):
//	  y   0..40   日付 YYYY/MM/DD(SAT) (Orbitron 32px、中央寄せ、下に細い罫線)
//	  y  59..180  時 / 分 の大カード (122x122) と 秒の縦長カード (62x122)
//	  y 190..240  左: バージョン表示 / 右: [SETUP] ボタン (どちらも main 側)
//
//	時・分の数字は LovyanGFX の Font8 (Arial 75px、数字専用) を等倍で使う。
//	等倍でしか綺麗に出ないフォントなので、カード幅の方を字に合わせてある
//	("88" = 55px x 2 = 110px)。秒は Font8 が入らないので、天地だけ時分と
//	揃えた縦長カードに FreeSansBold24pt を等倍で置く。
//
//	フリップの作り方:
//	  静止部  = 上半分に「新しい数字」、下半分に「古い数字」を置く
//	  前半    = 古いカードを垂直に縮めて上半分にだけ描く (上フラップが倒れる)
//	  後半    = 新しいカードを垂直に伸ばして下半分にだけ描く (下フラップが起きる)
//	いずれも割れ目を不動点にした拡縮なので pushRotateZoom 一発で描ける。
//	これに
//	  - 角度によるイージング (倒れるほど速い)
//	  - フラップが傾くほど暗くする陰影 (走査線間引き)
//	  - 相手側の面へ落ちる影
//	  - 動く縁のハイライト
//	を足してパタパタらしさを出している。
//
//	スプライトは一番大きいカードの寸法で3枚だけ確保し、小さいカードでは
//	その左上部分だけを使う (クリップして転送する)。
//
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>
#include "clock.h"
#include "display.h"

#define SCR_W 320
#define SCR_H 240
// 日付は上端いっぱいに詰める (罫線は引かない)
#define DATE_Y 2
#define DATE_H 32
// パタパタは日付のすぐ下。上下の余白は 4px だけ残す
#define CARD_Y 38               // 下端は 160
// 下 1/3 は世界時計 (コードのボタン + hh:mm を 6 個)
#define ZONE_Y 164
#define ZONE_W 53               // 6 列 x 53 = 318
#define ZONE_BTN_H 22
#define ZONE_TIME_Y (ZONE_Y + 30)

#define BIG_W 122               // 時・分のカード (Font8 の "88" = 110px + 余白)
#define BIG_H 122
#define SEC_W 62                // 秒のカード (時分と天地を揃えた縦長スリム)
#define SEC_H 122
#define CARD_GAP 6
#define ROW_W (BIG_W * 2 + SEC_W + CARD_GAP * 2)
#define ROW_X ((SCR_W - ROW_W) / 2)

#define FLIP_STEPS 7            // 片道のコマ数 (往復で 2倍)
#define FLIP_FRAME_MS 9
#define CARD_R 5                // 角丸の半径

//	配色: 黒のカードに白抜きの数字 (実物のパタパタ時計に合わせる)
#define C_BG        lgfx::color565(10, 11, 13)
#define C_DATERULE  lgfx::color565(60, 64, 70)
#define C_DATETX    lgfx::color565(196, 202, 210)
#define C_SUN       lgfx::color565(240, 90, 80)     // 日曜
#define C_SAT       lgfx::color565(90, 170, 255)    // 土曜
#define C_CARD_EDGE lgfx::color565(64, 66, 72)
#define C_BEVEL     lgfx::color565(92, 94, 100)     // カード上端のハイライト
#define C_CARD_SHDW lgfx::color565(4, 4, 5)         // カードの落ち影
#define C_HINGE     lgfx::color565(150, 152, 158)
#define C_SPLIT     lgfx::color565(0, 0, 0)
#define C_SPLIT_HI  lgfx::color565(70, 72, 78)
#define C_DIGIT     lgfx::color565(252, 252, 252)
#define C_SHADE     lgfx::color565(2, 2, 3)
#define C_EDGE_LIT  lgfx::color565(170, 172, 178)
#define C_TOAST_BG  lgfx::color565(20, 21, 24)
#define C_TOAST_BD  lgfx::color565(130, 134, 142)
#define C_ZONE_BG   lgfx::color565(22, 26, 34)      // ゾーンボタン (非選択)
#define C_ZONE_BD   lgfx::color565(60, 68, 82)
#define C_ZONE_TX   lgfx::color565(150, 160, 175)
#define C_ZONE_SBG  lgfx::color565(24, 54, 42)      // 選択中
#define C_ZONE_SBD  lgfx::color565(90, 200, 140)
#define C_ZONE_STX  lgfx::color565(200, 240, 215)
#define C_ZONE_TIME lgfx::color565(196, 202, 210)

// カード地のグラデーション (上端 -> 下端)
#define CARD_T_R 38
#define CARD_T_G 39
#define CARD_T_B 43
#define CARD_B_R 16
#define CARD_B_G 17
#define CARD_B_B 19

static const char *WDAY_EN[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

typedef struct {
	int16_t x, y, w, h;
	const lgfx::IFont *font;
	float scale;            // 数字の倍率 (基本は等倍)
	int8_t dy;              // 字面の中央を割れ目に合わせるための補正 (実測)
} card_t;

static card_t card[3] = {
	{ ROW_X,                          CARD_Y, BIG_W, BIG_H, &fonts::Font8, 1.0f, 0 },
	{ ROW_X + BIG_W + CARD_GAP,       CARD_Y, BIG_W, BIG_H, &fonts::Font8, 1.0f, 0 },
	{ ROW_X + (BIG_W + CARD_GAP) * 2, CARD_Y, SEC_W, SEC_H, &fonts::FreeSansBold24pt7b, 1.0f, 0 },
};

static LGFX *lcd;
static LGFX_Sprite spr_old, spr_new, spr_cmp;   // 大カード寸法で確保して使い回す
static bool sprites_ready = false;
static void (*redraw_hook)(void) = NULL;

// 時刻基準: epoch_base は millis()==base_ms のときの時刻
static uint32_t epoch_base;
static uint32_t base_ms;

static uint8_t shown[3] = { 0xFF, 0xFF, 0xFF };     // 表示中の 時/分/秒
static uint32_t shown_day = 0;                      // 表示中の日付 (epoch/86400)

//	----- 暦の計算 (Howard Hinnant の days_from_civil / civil_from_days) -----
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d);

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
	y -= (m <= 2);
	const int32_t era = (y >= 0 ? y : y - 399) / 400;
	const uint32_t yoe = (uint32_t)(y - era * 400);              // 0..399
	const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int16_t *y, uint8_t *m, uint8_t *d)
{
	z += 719468;
	const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
	const uint32_t doe = (uint32_t)(z - era * 146097);
	const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int32_t yr = (int32_t)yoe + era * 400;
	const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const uint32_t mp = (5 * doy + 2) / 153;
	const uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
	const uint32_t mm = mp + (mp < 10 ? 3 : -9);
	*y = (int16_t)(yr + (mm <= 2));
	*m = (uint8_t)mm;
	*d = (uint8_t)dd;
}

uint8_t clock_days_in_month(int16_t year, uint8_t mon)
{
	static const uint8_t t[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (mon == 2) {
		bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
		return leap ? 29 : 28;
	}
	return t[(mon - 1) % 12];
}

uint32_t clock_make(const clock_tm_t *tm)
{
	int32_t days = days_from_civil(tm->year, tm->mon, tm->day);
	return (uint32_t)days * 86400u + tm->hour * 3600u + tm->min * 60u + tm->sec;
}

void clock_break(uint32_t epoch, clock_tm_t *tm)
{
	uint32_t days = epoch / 86400u;
	uint32_t rem = epoch % 86400u;
	civil_from_days((int32_t)days, &tm->year, &tm->mon, &tm->day);
	tm->hour = (uint8_t)(rem / 3600u);
	tm->min = (uint8_t)((rem % 3600u) / 60u);
	tm->sec = (uint8_t)(rem % 60u);
	tm->wday = (uint8_t)((days + 4) % 7);       // 1970-01-01 は木曜
}

//==================================================================
//	世界時計のゾーンと夏時間
//	内部時刻は JST 基準。UTC = clock_now() - 9h を起点に各ゾーンを求める。
//==================================================================
#define CLOCK_BASE_MIN (9 * 60)         // 内部時刻の基準 (JST)

const clock_zone_t clock_zones[CLOCK_ZONE_N] = {
	{ "ZL",   12 * 60, CLOCK_DST_NZ },      // ニュージーランド
	{ "VK",   10 * 60, CLOCK_DST_AU },      // オーストラリア東部
	{ "JA",    9 * 60, CLOCK_DST_NONE },    // 日本
	{ "UTC",        0, CLOCK_DST_NONE },    // 世界標準時
	{ "W1",   -5 * 60, CLOCK_DST_US },      // 米国東部
	{ "W6",   -8 * 60, CLOCK_DST_US },      // 米国西部
};

static uint8_t zone_sel = CLOCK_ZONE_HOME;
static uint8_t summer_time = 1;
static uint8_t zone_shown[CLOCK_ZONE_N];    // 表示中の分 (書き換え判定用)
static uint8_t zone_shown_valid = 0;

//	year/mon の nth 番目の日曜 (nth=5 で最終日曜) の日
static uint8_t nth_sunday(int16_t year, uint8_t mon, uint8_t nth)
{
	int32_t first = days_from_civil(year, mon, 1);
	uint8_t wday = (uint8_t)((first + 4) % 7);          // 0=日
	uint8_t day = (uint8_t)(1 + ((7 - wday) % 7));      // その月の最初の日曜
	if (nth >= 5) {
		uint8_t last = clock_days_in_month(year, mon);
		while ((uint8_t)(day + 7) <= last) {
			day = (uint8_t)(day + 7);
		}
	} else {
		day = (uint8_t)(day + 7 * (nth - 1));
	}
	return day;
}

//	year/mon の nth 日曜 hour:00 の epoch (呼び出し側と同じ時間軸で)
static uint32_t rule_epoch(int16_t year, uint8_t mon, uint8_t nth, uint8_t hour)
{
	clock_tm_t t;
	t.year = year;
	t.mon = mon;
	t.day = nth_sunday(year, mon, nth);
	t.hour = hour;
	t.min = 0;
	t.sec = 0;
	t.wday = 0;
	return clock_make(&t);
}

//	夏時間の適用判定。loc は標準時のローカル時刻、utc は協定世界時
static bool dst_active(uint8_t rule, uint32_t utc, uint32_t loc)
{
	if (rule == CLOCK_DST_NONE || !summer_time) {
		return false;
	}
	clock_tm_t t;
	clock_break(loc, &t);
	int16_t y = t.year;
	switch (rule) {
	case CLOCK_DST_US:      // 3月第2日曜 02:00 〜 11月第1日曜 02:00 (夏時間) = 01:00 標準時
		return (loc >= rule_epoch(y, 3, 2, 2) && loc < rule_epoch(y, 11, 1, 1));
	case CLOCK_DST_EU:      // 3月最終日曜 01:00UTC 〜 10月最終日曜 01:00UTC
		return (utc >= rule_epoch(y, 3, 5, 1) && utc < rule_epoch(y, 10, 5, 1));
	case CLOCK_DST_NZ:      // 南半球: 9月最終日曜 〜 翌4月第1日曜
		return (loc >= rule_epoch(y, 9, 5, 2) || loc < rule_epoch(y, 4, 1, 2));
	case CLOCK_DST_AU:      // 南半球: 10月第1日曜 〜 翌4月第1日曜
		return (loc >= rule_epoch(y, 10, 1, 2) || loc < rule_epoch(y, 4, 1, 2));
	default:
		return false;
	}
}

uint8_t clock_zone(void)
{
	return zone_sel;
}

void clock_set_zone(uint8_t idx)
{
	if (idx < CLOCK_ZONE_N) {
		zone_sel = idx;
	}
}

uint8_t clock_summer_time(void)
{
	return summer_time;
}

void clock_set_summer_time(uint8_t on)
{
	summer_time = on ? 1 : 0;
	Preferences prefs;
	if (prefs.begin("cwdec", false)) {
		prefs.putUChar("summer", summer_time);
		prefs.end();
	}
	zone_shown_valid = 0;
}

static void summer_time_load(void)
{
	Preferences prefs;
	if (prefs.begin("cwdec", false)) {
		if (prefs.isKey("summer")) {
			summer_time = prefs.getUChar("summer", 1);
		}
		prefs.end();
	}
}

uint32_t clock_zone_now(uint8_t idx)
{
	if (idx >= CLOCK_ZONE_N) {
		idx = CLOCK_ZONE_HOME;
	}
	uint32_t utc = clock_now() - (uint32_t)CLOCK_BASE_MIN * 60;
	const clock_zone_t *z = &clock_zones[idx];
	uint32_t loc = (uint32_t)((int32_t)utc + (int32_t)z->std_min * 60);
	if (dst_active(z->dst_rule, utc, loc)) {
		loc += 3600;
	}
	return loc;
}

//	そのゾーンのローカル時刻を指定して内部時刻を合わせる
void clock_set_zone_time(uint8_t idx, uint32_t local)
{
	if (idx >= CLOCK_ZONE_N) {
		idx = CLOCK_ZONE_HOME;
	}
	const clock_zone_t *z = &clock_zones[idx];
	// 夏時間の判定は標準時ローカルで行う (入力値をそのまま使って近似)
	uint32_t utc = (uint32_t)((int32_t)local - (int32_t)z->std_min * 60);
	if (dst_active(z->dst_rule, utc, local)) {
		utc -= 3600;
	}
	clock_set(utc + (uint32_t)CLOCK_BASE_MIN * 60);
}

//	ビルド日時 (__DATE__ "Aug 16 2026" / __TIME__ "10:55:00") を基準時刻にする
static uint32_t build_epoch(void)
{
	static const char *mon_name = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon_s[4] = { __DATE__[0], __DATE__[1], __DATE__[2], 0 };
	const char *p = strstr(mon_name, mon_s);
	clock_tm_t tm = {};
	tm.mon = p ? (uint8_t)((p - mon_name) / 3 + 1) : 1;
	tm.day = (uint8_t)atoi(__DATE__ + 4);
	tm.year = (int16_t)atoi(__DATE__ + 7);
	tm.hour = (uint8_t)atoi(__TIME__);
	tm.min = (uint8_t)atoi(__TIME__ + 3);
	tm.sec = (uint8_t)atoi(__TIME__ + 6);
	return clock_make(&tm);
}

uint32_t clock_now(void)
{
	return epoch_base + (millis() - base_ms) / 1000u;
}

void clock_set(uint32_t epoch)
{
	epoch_base = epoch;
	base_ms = millis();
	shown[0] = shown[1] = shown[2] = 0xFF;      // 次の update で全カード描き直し
	shown_day = 0;
}

void clock_set_redraw_hook(void (*fn)(void))
{
	redraw_hook = fn;
}

//	----- 描画 -----

//	カード内に "88" が収まる倍率を求める。
//	ビットマップフォントは拡大すると粗くなるので等倍を上限にする。
static void fit_digits(card_t &c)
{
	spr_new.setFont(c.font);
	spr_new.setTextSize(1.0f);
	float w = spr_new.textWidth("88");
	float h = (float)spr_new.fontHeight();
	if (w < 1.0f || h < 1.0f) {
		return;
	}
	float s = (c.w - 8) / w;
	float sh = (c.h - 12) / h;
	if (sh < s) {
		s = sh;
	}
	c.scale = (s > 1.0f) ? 1.0f : s;
}

//	"88" を実際に描いてインクの上端・下端を測り、
//	割れ目 (カード中央) が数字の高さの中央に一致する描画位置を求める。
//	フォントの箱の中央と字面の中央はズレる (数字に下がり部が無い) ので実測する。
static void measure_digits(card_t &c)
{
	const int half = c.h / 2;
	spr_cmp.fillSprite(0);
	spr_cmp.setFont(c.font);
	spr_cmp.setTextSize(c.scale);
	spr_cmp.setTextDatum(lgfx::textdatum_t::middle_center);
	spr_cmp.setTextColor(0xFFFF);
	spr_cmp.drawString("88", c.w / 2, half);
	spr_cmp.setTextDatum(lgfx::textdatum_t::top_left);
	spr_cmp.setTextSize(1.0f);

	int top = -1, bot = -1;
	for (int y = 0; y < c.h; y++) {
		for (int x = 0; x < c.w; x += 2) {
			if (spr_cmp.readPixel(x, y)) {
				if (top < 0) {
					top = y;
				}
				bot = y;
				break;
			}
		}
	}
	c.dy = (top >= 0) ? (int8_t)(half - (top + bot) / 2) : 0;
}

static uint16_t mix565(int r0, int g0, int b0, int r1, int g1, int b1, float t)
{
	return lgfx::color565((uint8_t)(r0 + (r1 - r0) * t),
	                      (uint8_t)(g0 + (g1 - g0) * t),
	                      (uint8_t)(b0 + (b1 - b0) * t));
}

//	角丸のためにその行で削るピクセル数
static int corner_inset(int y, int h)
{
	int d = 0;
	if (y < CARD_R) {
		d = CARD_R - y;
	} else if (y >= h - CARD_R) {
		d = CARD_R - (h - 1 - y);
	}
	if (d <= 0) {
		return 0;
	}
	return CARD_R - (int)(sqrtf((float)(CARD_R * CARD_R - d * d)) + 0.5f);
}

//	カード1枚の絵をスプライトの左上へ描く (数字は割れ目をまたぐ)
static void render_card(const card_t &c, LGFX_Sprite &s, uint8_t value)
{
	const int half = c.h / 2;

	for (int y = 0; y < c.h; y++) {
		int in = corner_inset(y, c.h);
		s.drawFastHLine(in, y, c.w - in * 2,
		                mix565(CARD_T_R, CARD_T_G, CARD_T_B,
		                       CARD_B_R, CARD_B_G, CARD_B_B, (float)y / c.h));
	}

	char buf[4];
	snprintf(buf, sizeof(buf), "%02u", value % 100);
	s.setFont(c.font);
	s.setTextSize(c.scale);
	s.setTextDatum(lgfx::textdatum_t::middle_center);
	s.setTextColor(C_DIGIT);
	s.drawString(buf, c.w / 2, half + c.dy);
	s.setTextDatum(lgfx::textdatum_t::top_left);
	s.setTextSize(1.0f);

	s.fillRect(0, half - 1, c.w, 2, C_SPLIT);                   // 割れ目
	s.drawFastHLine(0, half + 1, c.w, C_SPLIT_HI);              // 割れ目直下の反射
	s.fillRect(0, half - 6, 5, 12, C_HINGE);                    // ヒンジ (左右)
	s.fillRect(c.w - 5, half - 6, 5, 12, C_HINGE);
	s.drawRoundRect(0, 0, c.w, c.h, CARD_R, C_CARD_EDGE);
	s.drawFastHLine(CARD_R, 1, c.w - CARD_R * 2, C_BEVEL);      // 上端のハイライト
}

//	傾いたフラップを暗くする: 走査線を間引いて描き潰す (擬似的な陰影)
static void shade_flap(const card_t &c, int y0, int h, float dark)
{
	if (h <= 0 || dark <= 0.15f) {
		return;
	}
	int step = (dark > 0.66f) ? 2 : (dark > 0.40f) ? 3 : 4;
	for (int y = y0; y < y0 + h; y += step) {
		spr_cmp.drawFastHLine(0, y, c.w, C_SHADE);
	}
}

//	スプライトはカードより大きいことがあるので、カード矩形だけ転送する
static void push_card(const card_t &c, LGFX_Sprite &s)
{
	lcd->setClipRect(c.x, c.y, c.w, c.h);
	s.pushSprite(lcd, c.x, c.y);
	lcd->clearClipRect();
}

//	1枚ぶんのフリップ動作 (約130ms、その間は他の処理を止める)
static void flip_card(const card_t &c, uint8_t from, uint8_t to)
{
	if (!sprites_ready) {
		return;
	}
	render_card(c, spr_old, from);
	render_card(c, spr_new, to);

	const int half = c.h / 2;
	const int shadow = c.h / 6;
	// 拡縮の不動点 (割れ目) をスプライトの回転中心にする
	spr_old.setPivot(c.w / 2.0f, (float)half);
	spr_new.setPivot(c.w / 2.0f, (float)half);

	for (int i = 0; i <= FLIP_STEPS * 2; i++) {
		bool folding = (i <= FLIP_STEPS);
		// 角度で進める: 倒れ際が速くなり、実物の落ち方に近くなる
		float ang = folding ? (float)i / FLIP_STEPS * (float)M_PI_2
		                    : (float)(FLIP_STEPS * 2 - i) / FLIP_STEPS * (float)M_PI_2;
		float s = cosf(ang);
		if (s < 0.0f) s = 0.0f;

		// 静止部: 上半分 = 新しい数字 / 下半分 = 古い数字
		spr_cmp.setClipRect(0, 0, c.w, half);
		spr_new.pushSprite(&spr_cmp, 0, 0);
		spr_cmp.setClipRect(0, half, c.w, c.h - half);
		spr_old.pushSprite(&spr_cmp, 0, 0);
		spr_cmp.clearClipRect();

		int flap_h = (int)(half * s + 0.5f);
		float dark = 1.0f - s;

		if (folding) {
			// 落ちてくる上フラップ + 下半分に伸びる影
			shade_flap(c, half, (int)((1.0f - s) * shadow), 0.9f);
			if (flap_h > 1) {
				spr_cmp.setClipRect(0, 0, c.w, half);
				spr_old.pushRotateZoom(&spr_cmp, c.w / 2.0f, (float)half, 0.0f, 1.0f, s);
				shade_flap(c, half - flap_h, flap_h, dark);
				spr_cmp.drawFastHLine(0, half - flap_h, c.w, C_EDGE_LIT);
				spr_cmp.clearClipRect();
			}
		} else {
			// 起き上がる下フラップ + 上半分へ落ちる影
			shade_flap(c, half - (int)((1.0f - s) * shadow), (int)((1.0f - s) * shadow), 0.9f);
			if (flap_h > 1) {
				spr_cmp.setClipRect(0, half, c.w, c.h - half);
				spr_new.pushRotateZoom(&spr_cmp, c.w / 2.0f, (float)half, 0.0f, 1.0f, s);
				shade_flap(c, half, flap_h, dark);
				spr_cmp.drawFastHLine(0, half + flap_h - 1, c.w, C_EDGE_LIT);
				spr_cmp.clearClipRect();
			}
		}
		push_card(c, spr_cmp);
		delay(FLIP_FRAME_MS);
	}
	push_card(c, spr_new);
}

static void draw_card_static(const card_t &c, uint8_t value)
{
	if (!sprites_ready) {
		return;
	}
	render_card(c, spr_new, value);
	push_card(c, spr_new);
}

//	日付: Orbitron で YYYY/MM/DD(SAT) と、右に選択中のゾーンのコード。
//	上端いっぱいに詰め、罫線は引かない。全体をまとめて中央寄せする。
static void draw_date(const clock_tm_t *tm)
{
	lcd->fillRect(0, 0, SCR_W, DATE_Y + DATE_H, C_BG);

	char head[24], tail[8];
	snprintf(head, sizeof(head), "%04d/%02d/%02d", tm->year, tm->mon, tm->day);
	snprintf(tail, sizeof(tail), "(%s)", WDAY_EN[tm->wday]);
	uint16_t wcol = (tm->wday == 0) ? C_SUN
	              : (tm->wday == 6) ? C_SAT : C_DATETX;
	const char *code = clock_zones[zone_sel].code;

	// Orbitron は字幅が広い。32px で収まらなければ 24px に落とす
	const int gap = 8;              // 日付と (曜日) の間
	const int gap2 = 12;            // (曜日) とコードの間
	lcd->setFont(&fonts::Orbitron_Light_32);
	int wh = lcd->textWidth(head);
	int wt = lcd->textWidth(tail);
	lcd->setFont(&fonts::AsciiFont8x16);
	int wc = lcd->textWidth(code);
	if (wh + gap + wt + gap2 + wc > SCR_W - 8) {
		lcd->setFont(&fonts::Orbitron_Light_24);
		wh = lcd->textWidth(head);
		wt = lcd->textWidth(tail);
	}
	int x = (SCR_W - (wh + gap + wt + gap2 + wc)) / 2;
	if (x < 4) {
		x = 4;
	}
	const int cy = DATE_Y + DATE_H / 2;

	lcd->setTextDatum(lgfx::textdatum_t::middle_left);
	lcd->setTextColor(C_DATETX, C_BG);
	lcd->drawString(head, x, cy);
	lcd->setTextColor(wcol, C_BG);
	lcd->drawString(tail, x + wh + gap, cy);
	lcd->setFont(&fonts::AsciiFont8x16);
	lcd->setTextColor(C_ZONE_SBD, C_BG);
	lcd->drawString(code, x + wh + gap + wt + gap2, cy);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

//==================================================================
//	世界時計の行 (コードのボタン + hh:mm を 6 個、朝が早い順)
//==================================================================
static void draw_zone_cell(uint8_t i, bool force)
{
	clock_tm_t t;
	clock_break(clock_zone_now(i), &t);
	if (!force && zone_shown_valid && zone_shown[i] == t.min) {
		return;
	}
	zone_shown[i] = t.min;

	const int x = 1 + i * ZONE_W;
	const bool sel = (i == zone_sel);

	if (force) {
		lcd->fillRect(x, ZONE_Y, ZONE_W, SCR_H - ZONE_Y, C_BG);
		lcd->fillRoundRect(x + 2, ZONE_Y, ZONE_W - 5, ZONE_BTN_H, 4,
		                   sel ? C_ZONE_SBG : C_ZONE_BG);
		lcd->drawRoundRect(x + 2, ZONE_Y, ZONE_W - 5, ZONE_BTN_H, 4,
		                   sel ? C_ZONE_SBD : C_ZONE_BD);
		lcd->setFont(&fonts::AsciiFont8x16);
		lcd->setTextColor(sel ? C_ZONE_STX : C_ZONE_TX);
		lcd->setTextDatum(lgfx::textdatum_t::middle_center);
		lcd->drawString(clock_zones[i].code, x + ZONE_W / 2 - 1, ZONE_Y + ZONE_BTN_H / 2);
	}

	char buf[8];
	snprintf(buf, sizeof(buf), "%02u:%02u", t.hour, t.min);
	lcd->fillRect(x, ZONE_TIME_Y, ZONE_W, 16, C_BG);
	lcd->setFont(&fonts::AsciiFont8x16);
	lcd->setTextColor(sel ? C_ZONE_SBD : C_ZONE_TIME);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString(buf, x + ZONE_W / 2 - 1, ZONE_TIME_Y + 8);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void draw_zones(bool force)
{
	for (uint8_t i = 0; i < CLOCK_ZONE_N; i++) {
		draw_zone_cell(i, force);
	}
	zone_shown_valid = 1;
}

//	タップ位置からゾーンを選ぶ。切り替えたら true
bool clock_zone_touch(int32_t tx, int32_t ty)
{
	if (ty < ZONE_Y || ty >= SCR_H) {
		return false;
	}
	int i = (tx - 1) / ZONE_W;
	if (i < 0 || i >= CLOCK_ZONE_N || (uint8_t)i == zone_sel) {
		return false;
	}
	zone_sel = (uint8_t)i;
	draw_zones(true);               // 選択枠を描き直す
	clock_tm_t tm;
	clock_break(clock_zone_now(zone_sel), &tm);
	draw_date(&tm);                 // 日付とコードも更新
	shown_day = clock_zone_now(zone_sel) / 86400u;
	return true;                    // カードは clock_update がパタパタで追従する
}

void clock_toast(const char *msg, uint16_t color)
{
	const int x = 20, y = 96, w = 280, h = 48;
	lcd->fillRect(x, y, w, h, C_TOAST_BG);
	lcd->drawRect(x, y, w, h, C_TOAST_BD);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(color, C_TOAST_BG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString(msg, x + w / 2, y + h / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

void clock_redraw(void)
{
	lcd->fillScreen(C_BG);
	clock_tm_t tm;
	clock_break(clock_zone_now(zone_sel), &tm);
	draw_date(&tm);
	for (int i = 0; i < 3; i++) {           // カードの落ち影
		lcd->fillRoundRect(card[i].x + 2, card[i].y + 3, card[i].w, card[i].h,
		                   CARD_R, C_CARD_SHDW);
	}
	zone_shown_valid = 0;
	draw_zones(true);
	shown[0] = shown[1] = shown[2] = 0xFF;
	shown_day = clock_zone_now(zone_sel) / 86400u;
	if (redraw_hook) {
		redraw_hook();
	}
	clock_update();
}

//	スプライトをデコーダ画面と共有のバッファ (display_sprite_arena) に割り付ける。
//	時計画面に入るたびに呼ぶ (デコーダ側が同じメモリを使ったあとなので
//	指し直す)。字面の実測は初回だけ行う。
bool clock_alloc(void)
{
	static bool measured = false;
	const size_t n_card = SPRITE_BYTES_16(BIG_W, BIG_H);
	uint8_t *base = (uint8_t *)display_sprite_arena(n_card * 3);
	if (!base) {
		sprites_ready = false;
		Serial.println("[clock] sprite arena unavailable");
		return false;
	}
	spr_old.setColorDepth(16);
	spr_old.setBuffer(base, BIG_W, BIG_H);
	spr_new.setColorDepth(16);
	spr_new.setBuffer(base + n_card, BIG_W, BIG_H);
	spr_cmp.setColorDepth(16);
	spr_cmp.setBuffer(base + n_card * 2, BIG_W, BIG_H);
	sprites_ready = true;
	if (!measured) {
		measured = true;
		for (int i = 0; i < 3; i++) {
			fit_digits(card[i]);
			measure_digits(card[i]);
		}
		Serial.printf("[clock] scale %.2f/%.2f/%.2f  split %d/%d/%d  heap=%u\n",
		              card[0].scale, card[1].scale, card[2].scale,
		              card[0].dy, card[1].dy, card[2].dy, (unsigned)ESP.getFreeHeap());
	}
	return true;
}

//	時刻の基準だけ用意する。スプライト確保と描画は時計画面に入るときに
//	clock_alloc() / clock_redraw() で行う (デコーダ画面ではメモリを使わない)。
void clock_init(LGFX *lcd_)
{
	lcd = lcd_;
	epoch_base = build_epoch();
	base_ms = millis();
	summer_time_load();
}

//	共有バッファは解放しない (デコーダ画面が使う)。描画だけ止める
void clock_free(void)
{
	sprites_ready = false;
}

void clock_update(void)
{
	clock_tm_t tm;
	uint32_t now = clock_zone_now(zone_sel);
	clock_break(now, &tm);

	if (now / 86400u != shown_day) {
		shown_day = now / 86400u;
		draw_date(&tm);
	}
	draw_zones(false);              // 分が変わった枠だけ描き直す

	const uint8_t val[3] = { tm.hour, tm.min, tm.sec };
	for (int i = 0; i < 3; i++) {
		if (val[i] == shown[i]) {
			continue;
		}
		if (shown[i] == 0xFF) {
			draw_card_static(card[i], val[i]);      // 初回 / 再描画はアニメ無し
		} else {
			flip_card(card[i], shown[i], val[i]);
		}
		shown[i] = val[i];
	}
}
