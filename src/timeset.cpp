//
//	セットアップ画面 (時刻合わせ)
//
//	  年     月    日    時    分
//	  ▲     ▲    ▲    ▲    ▲
//	 2026    08    16    10    55
//	  ▼     ▼    ▼    ▼    ▼
//	      [キャンセル]      [決定]
//
//	秒は「決定」を押した瞬間に 0 にする。
//	▲▼ は長押しでリピートする。
//
#include <Arduino.h>
#include "timeset.h"
#include "clock.h"

#define C_BG      lgfx::color565(14, 17, 22)
#define C_TITLEBG lgfx::color565(26, 34, 46)
#define C_TITLETX lgfx::color565(200, 212, 226)
#define C_LABEL   lgfx::color565(120, 140, 165)
#define C_VALUE   lgfx::color565(235, 240, 246)
#define C_FIELDBG lgfx::color565(22, 30, 42)
#define C_FIELDBD lgfx::color565(51, 69, 92)
#define C_BTN_BG  lgfx::color565(26, 34, 46)
#define C_BTN_BD  lgfx::color565(70, 92, 120)
#define C_BTN_TX  lgfx::color565(190, 206, 226)
#define C_OK_BG   lgfx::color565(24, 54, 42)
#define C_OK_BD   lgfx::color565(90, 200, 140)

#define LABEL_Y 34
#define UP_Y    52
#define ARROW_H 34
#define VAL_Y   90
#define VAL_H   38
#define DN_Y    130
#define BTN_Y   186
#define BTN_H   40

#define FIELD_N 5

static const struct { int16_t x, w; const char *label; } FIELD[FIELD_N] = {
	{  10, 76, "年" },
	{  96, 46, "月" },
	{ 152, 46, "日" },
	{ 208, 46, "時" },
	{ 264, 46, "分" },
};

static LGFX *lcd;
static clock_tm_t tm;

static bool hit(int tx, int ty, int x, int y, int w, int h)
{
	return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

static void draw_button(int x, int y, int w, int h, const char *label,
                        uint16_t bg, uint16_t bd, uint16_t tx)
{
	lcd->fillRoundRect(x, y, w, h, 5, bg);
	lcd->drawRoundRect(x, y, w, h, 5, bd);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(tx, bg);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString(label, x + w / 2, y + h / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void draw_arrow(int idx, bool up)
{
	const int x = FIELD[idx].x, w = FIELD[idx].w;
	const int y = up ? UP_Y : DN_Y;
	lcd->fillRoundRect(x, y, w, ARROW_H, 5, C_BTN_BG);
	lcd->drawRoundRect(x, y, w, ARROW_H, 5, C_BTN_BD);
	const int cx = x + w / 2, cy = y + ARROW_H / 2;
	if (up) {
		lcd->fillTriangle(cx, cy - 8, cx - 10, cy + 6, cx + 10, cy + 6, C_BTN_TX);
	} else {
		lcd->fillTriangle(cx, cy + 8, cx - 10, cy - 6, cx + 10, cy - 6, C_BTN_TX);
	}
}

static void draw_value(int idx)
{
	const int x = FIELD[idx].x, w = FIELD[idx].w;
	lcd->fillRoundRect(x, VAL_Y, w, VAL_H, 4, C_FIELDBG);
	lcd->drawRoundRect(x, VAL_Y, w, VAL_H, 4, C_FIELDBD);
	lcd->setFont(&fonts::DejaVu24);
	lcd->setTextColor(C_VALUE, C_FIELDBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	char buf[8];
	switch (idx) {
	case 0: snprintf(buf, sizeof(buf), "%04d", tm.year); break;
	case 1: snprintf(buf, sizeof(buf), "%02d", tm.mon); break;
	case 2: snprintf(buf, sizeof(buf), "%02d", tm.day); break;
	case 3: snprintf(buf, sizeof(buf), "%02d", tm.hour); break;
	default: snprintf(buf, sizeof(buf), "%02d", tm.min); break;
	}
	lcd->drawString(buf, x + w / 2, VAL_Y + VAL_H / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void bump(int idx, int dir)
{
	switch (idx) {
	case 0:
		tm.year += dir;
		if (tm.year < 2020) tm.year = 2020;
		if (tm.year > 2099) tm.year = 2099;
		break;
	case 1:
		tm.mon = (uint8_t)((tm.mon - 1 + 12 + dir) % 12 + 1);
		break;
	case 2:
		{
			uint8_t last = clock_days_in_month(tm.year, tm.mon);
			int d = tm.day + dir;
			if (d < 1) d = last;
			if (d > last) d = 1;
			tm.day = (uint8_t)d;
		}
		return;
	case 3:
		tm.hour = (uint8_t)((tm.hour + 24 + dir) % 24);
		return;
	default:
		tm.min = (uint8_t)((tm.min + 60 + dir) % 60);
		return;
	}
	// 年 / 月 を動かしたら日を月末に丸める
	uint8_t last = clock_days_in_month(tm.year, tm.mon);
	if (tm.day > last) {
		tm.day = last;
		draw_value(2);
	}
}

static void draw_screen(void)
{
	lcd->fillScreen(C_BG);

	lcd->fillRect(0, 0, 320, 34, C_TITLEBG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_TITLETX, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_left);
	lcd->drawString("時刻合わせ", 8, 17);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_LABEL, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	for (int i = 0; i < FIELD_N; i++) {
		lcd->drawString(FIELD[i].label, FIELD[i].x + FIELD[i].w / 2, LABEL_Y);
	}
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	for (int i = 0; i < FIELD_N; i++) {
		draw_arrow(i, true);
		draw_value(i);
		draw_arrow(i, false);
	}
	draw_button(30, BTN_Y, 110, BTN_H, "キャンセル", C_BTN_BG, C_BTN_BD, C_BTN_TX);
	draw_button(180, BTN_Y, 110, BTN_H, "決定", C_OK_BG, C_OK_BD, C_BTN_TX);
}

bool timeset_run(LGFX *lcd_)
{
	lcd = lcd_;
	clock_break(clock_now(), &tm);
	draw_screen();

	int32_t tx, ty;
	while (lcd->getTouch(&tx, &ty)) {
		delay(10);                  // 前のタッチが離れるのを待つ
	}

	int held = -1;                  // 長押し中の ▲▼ (idx*2 + (0:up/1:down))
	uint32_t held_ms = 0;
	uint32_t repeat_ms = 0;

	for (;;) {
		if (!lcd->getTouch(&tx, &ty)) {
			held = -1;
			delay(10);
			continue;
		}

		// 決定 / キャンセル
		if (hit(tx, ty, 180, BTN_Y, 110, BTN_H)) {
			while (lcd->getTouch(&tx, &ty)) delay(10);
			tm.sec = 0;
			clock_set(clock_make(&tm));
			return true;
		}
		if (hit(tx, ty, 30, BTN_Y, 110, BTN_H)) {
			while (lcd->getTouch(&tx, &ty)) delay(10);
			return false;
		}

		// ▲▼ (押し始めで1回、長押しでリピート)
		int idx = -1, dir = 0;
		for (int i = 0; i < FIELD_N; i++) {
			if (hit(tx, ty, FIELD[i].x, UP_Y, FIELD[i].w, ARROW_H)) { idx = i; dir = +1; break; }
			if (hit(tx, ty, FIELD[i].x, DN_Y, FIELD[i].w, ARROW_H)) { idx = i; dir = -1; break; }
		}
		if (idx < 0) {
			held = -1;
			delay(10);
			continue;
		}

		int key = idx * 2 + (dir > 0 ? 0 : 1);
		uint32_t now = millis();
		bool fire = false;
		if (key != held) {
			held = key;
			held_ms = now;
			repeat_ms = now;
			fire = true;
		} else if (now - held_ms > 500 && now - repeat_ms > 120) {
			repeat_ms = now;
			fire = true;
		}
		if (fire) {
			bump(idx, dir);
			draw_value(idx);
		}
		delay(10);
	}
}
