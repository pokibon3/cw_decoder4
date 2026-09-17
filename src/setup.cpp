//
//	セットアップ画面
//
//	┌ SETUP                               [戻る] ┐
//	│      [ WiFi設定 (NTP時刻同期用) ]          │
//	│      [ 時刻合わせ ]                        │
//	│      [ Summer Time: OFF/ON ]               │
//	│      [ スコープログ: OFF/ON ]              │
//	│      [ ファームウェアアップデート ]        │
//	│      [ 初期化 (WiFi設定等) ]               │
//	│      [ CW Decoder について ]  → About      │
//	└────────────────────────────────────────────┘
//
#include <Arduino.h>
#include <WiFi.h>
#include "setup.h"
#include "clock.h"
#include "timeset.h"
#include "netsync.h"
#include "ota.h"
#include "dsp.h"
#include "version.h"
#include "scopelog.h"
#include "display.h"
#include <esp_ota_ops.h>

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
#define C_DLG_BG  lgfx::color565(20, 27, 37)
#define C_DLG_BD  lgfx::color565(120, 150, 190)

#define TITLE_H 36
#define BACK_X 248
#define BACK_Y 4
#define BACK_W 64
#define BACK_H 26

#define ITEM_X 20
#define ITEM_W 280
#define ITEM_H 24
#define ITEM_Y0 42
#define ITEM_PITCH 28
#define ITEM_Y(i) (ITEM_Y0 + ITEM_PITCH * (i))
#define ITEM_N 7

// About 画面 (スプラッシュと同じ体裁)
#define ABOUT_BACK_X 214
#define ABOUT_BACK_Y 206
#define ABOUT_BACK_W 92
#define ABOUT_BACK_H 28

static LGFX *lcd;

static bool hit(int tx, int ty, int x, int y, int w, int h)
{
	return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

static void draw_button(int x, int y, int w, int h, const char *label,
                        uint16_t bg, uint16_t bd, uint16_t tx, const lgfx::IFont *font)
{
	lcd->fillRoundRect(x, y, w, h, 6, bg);
	lcd->drawRoundRect(x, y, w, h, 6, bd);
	lcd->setFont(font);
	lcd->setTextColor(tx, bg);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString(label, x + w / 2, y + h / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

static void wait_release(void)
{
	int32_t tx, ty;
	while (lcd->getTouch(&tx, &ty)) {
		delay(10);
	}
}

//==================================================================
//	About 画面: スプラッシュと同じ見出しに詳細情報を足したもの
//==================================================================
static void about_run(void)
{
	lcd->fillScreen(DISPLAY_SPLASH_BG);
	display_splash_header(26);

	char buf[80];
	const esp_partition_t *run = esp_ota_get_running_partition();
	struct { const char *label; String value; } rows[6];
	rows[0] = { "Version", String(FW_VERSION) };
	rows[1] = { "Build", String(FW_BUILD) };
	snprintf(buf, sizeof(buf), "%s / LovyanGFX", lcd->panel_name());
	rows[2] = { "Panel", String(buf) };
	snprintf(buf, sizeof(buf), "%s  heap %uKB", run ? run->label : "?",
	         (unsigned)(ESP.getFreeHeap() / 1024));
	rows[3] = { "Boot", String(buf) };
	rows[4] = { "WiFi", netsync_has_wifi() ? netsync_ssid() : String("未設定") };
	if (netsync_last_epoch()) {
		clock_tm_t t;
		clock_break(netsync_last_epoch(), &t);
		snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d 同期済", t.mon, t.day, t.hour, t.min);
		rows[5] = { "NTP", String(buf) };
	} else {
		rows[5] = { "NTP", String("未同期") };
	}

	int y = 102;
	for (auto &r : rows) {
		lcd->setFont(&fonts::lgfxJapanGothicP_16);
		lcd->setTextColor(C_LABEL, DISPLAY_SPLASH_BG);
		lcd->drawString(r.label, 24, y);
		lcd->setTextColor(C_VALUE, DISPLAY_SPLASH_BG);
		lcd->setClipRect(96, y, 320 - 96 - 8, 18);
		lcd->drawString(r.value, 96, y);
		lcd->clearClipRect();
		y += 17;
	}

	draw_button(ABOUT_BACK_X, ABOUT_BACK_Y, ABOUT_BACK_W, ABOUT_BACK_H, "戻る",
	            C_BTN_BG, C_BTN_BD, C_BTN_TX, &fonts::lgfxJapanGothicP_16);

	wait_release();
	int32_t tx, ty;
	for (;;) {                      // どこをタップしても戻る
		if (lcd->getTouch(&tx, &ty)) {
			wait_release();
			return;
		}
		delay(10);
	}
}

static void draw_screen(void)
{
	lcd->fillScreen(C_BG);

	lcd->fillRect(0, 0, 320, TITLE_H, C_TITLEBG);
	lcd->setFont(&fonts::Orbitron_Light_24);
	lcd->setTextColor(C_TITLETX, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_left);
	lcd->drawString("SETUP", 8, TITLE_H / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
	draw_button(BACK_X, BACK_Y, BACK_W, BACK_H, "戻る", C_BTN_BG, C_BTN_BD, C_BTN_TX,
	            &fonts::lgfxJapanGothicP_16);

	char summer[40], scope[40];
	snprintf(summer, sizeof(summer), "Summer Time: %s", clock_summer_time() ? "ON" : "OFF");
	snprintf(scope, sizeof(scope), "スコープログ: %s",
	         scopelog_enabled() ? "ON (シリアル)" : "OFF");
	const struct { const char *label; bool on; } items[ITEM_N] = {
		{ "WiFi設定 (NTP時刻同期用)", false },
		{ "時刻合わせ", false },
		{ summer, clock_summer_time() != 0 },
		{ scope, scopelog_enabled() != 0 },
		{ "ファームウェアアップデート", false },
		{ "初期化 (WiFi設定等)", false },
		{ "CW Decoder について", false },
	};
	for (int i = 0; i < ITEM_N; i++) {
		draw_button(ITEM_X, ITEM_Y(i), ITEM_W, ITEM_H, items[i].label,
		            items[i].on ? C_OK_BG : C_BTN_BG,
		            items[i].on ? C_OK_BD : C_BTN_BD, C_BTN_TX,
		            &fonts::lgfxJapanGothicP_16);
	}
}

//	確認ダイアログ (title / 2行の説明 / キャンセル・ok_label)
static bool confirm(const char *title, const char *line1, const char *line2,
                    const char *ok_label)
{
	const int x = 24, y = 52, w = 272, h = 136;
	lcd->fillRoundRect(x, y, w, h, 8, C_DLG_BG);
	lcd->drawRoundRect(x, y, w, h, 8, C_DLG_BD);

	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_VALUE, C_DLG_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	lcd->drawString(title, x + w / 2, y + 14);
	lcd->setTextColor(C_LABEL, C_DLG_BG);
	lcd->drawString(line1, x + w / 2, y + 44);
	lcd->drawString(line2, x + w / 2, y + 64);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	const int bw = 112, bh = 36, by = y + h - bh - 14;
	const int bx_no = x + 16, bx_yes = x + w - bw - 16;
	draw_button(bx_no, by, bw, bh, "キャンセル", C_BTN_BG, C_BTN_BD, C_BTN_TX,
	            &fonts::lgfxJapanGothicP_16);
	draw_button(bx_yes, by, bw, bh, ok_label, C_OK_BG, C_OK_BD, C_BTN_TX,
	            &fonts::lgfxJapanGothicP_16);

	wait_release();
	int32_t tx, ty;
	for (;;) {
		if (lcd->getTouch(&tx, &ty)) {
			bool yes = hit(tx, ty, bx_yes, by, bw, bh);
			bool no = hit(tx, ty, bx_no, by, bw, bh);
			wait_release();
			if (yes) return true;
			if (no) return false;
		}
		delay(10);
	}
}

//	AP (WiFi) を使うモードの前後処理: ADC DMA を止めて DSP を待機させる
//	(WiFi と同時に動かすと WDT リセット)。スプライト類は解放しない —
//	DMA 対応メモリが WiFi 使用後に断片化し、作り直しに失敗するため
static void ap_mode_enter(void)
{
	dsp_set_paused(1);
}

static void ap_mode_leave(void)
{
	dsp_set_paused(0);
}

void setup_run(LGFX *lcd_)
{
	lcd = lcd_;
	draw_screen();
	wait_release();

	int32_t tx, ty;
	for (;;) {
		if (!lcd->getTouch(&tx, &ty)) {
			delay(10);
			continue;
		}
		if (hit(tx, ty, BACK_X, BACK_Y, BACK_W, BACK_H)) {
			wait_release();
			return;
		}
		int idx = -1;
		for (int i = 0; i < ITEM_N; i++) {
			if (hit(tx, ty, ITEM_X, ITEM_Y(i), ITEM_W, ITEM_H)) {
				idx = i;
				break;
			}
		}
		wait_release();
		if (idx < 0) {
			continue;
		}
		switch (idx) {
		case 0:                     // WiFi設定 (AP を立ててブラウザから登録)
			ap_mode_enter();
			wifi_setup_run(lcd);
			ap_mode_leave();
			break;
		case 1:
			timeset_run(lcd);
			break;
		case 2:
			clock_set_summer_time(!clock_summer_time());
			break;
		case 3:
			scopelog_set_enabled(!scopelog_enabled());
			break;
		case 4:
			if (confirm("OTAモードに入ります",
			            "受信を止めて WiFi を起動します",
			            "更新が成功すると再起動します", "開始")) {
				ap_mode_enter();
				ota_run(lcd);       // 成功時は再起動、キャンセルで戻る
				ap_mode_leave();
			}
			break;
		case 5:
			if (confirm("設定を初期化します",
			            "WiFi の SSID / パスワードを消し",
			            "NTP 時刻同期を止めます", "初期化")) {
				netsync_clear_wifi();
			}
			break;
		default:
			about_run();
			break;
		}
		draw_screen();
	}
}
