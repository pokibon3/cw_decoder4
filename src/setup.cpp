//
//	セットアップ画面
//
//	2列 x 5行のグリッド (9項目、最終行の右列は空き)。
//
//	┌ SETUP                                [戻る] ┐
//	│ [ WiFi設定 ]         [ 時刻合わせ ]         │
//	│ [ FW更新 ]           [ 夏時間: ON ]         │
//	│ [ FW自動更新: ON ]   [ タッチパネル調整 ]   │
//	│ [ ログ出力: ON ]     [ 初期化 ]             │
//	│ [ About ]                                   │
//	└─────────────────────────────────────────────┘
//
//	FW更新 → ota (SoftAP+ブラウザで手動アップロード)
//	タッチパネル調整 → touchcal / About → about_run
//
//	「自動更新」は起動時 (スプラッシュ表示中) に WiFi 経由で新しい
//	ファームウェアを自動チェックするかどうかの ON/OFF (fwupdate.cpp)。
//	「FW更新」は既存の SoftAP+ブラウザ手動アップロード (ota.cpp)。
//
#include <Arduino.h>
#include <WiFi.h>
#include "setup.h"
#include "clock.h"
#include "timeset.h"
#include "netsync.h"
#include "ota.h"
#include "fwupdate.h"
#include "dsp.h"
#include "version.h"
#include "scopelog.h"
#include "touchcal.h"
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

#define TITLE_H 36
#define BACK_X 248
#define BACK_Y 4
#define BACK_W 64
#define BACK_H 26

// 2列 x 5行のグリッド (9項目、最終行の右列だけ空き)
#define ITEM_COL_W 138
#define ITEM_COL_GAP 12
#define ITEM_H 34
#define ITEM_Y0 40
#define ITEM_PITCH 38
#define ITEM_N 9
#define ITEM_COL_X(c) (16 + (c) * (ITEM_COL_W + ITEM_COL_GAP))
#define ITEM_ROW_Y(r) (ITEM_Y0 + ITEM_PITCH * (r))

// idx (0..ITEM_N-1) から (x, y) を求める。row = idx/2, col = idx%2
static void item_pos(int idx, int *x, int *y)
{
	*x = ITEM_COL_X(idx % 2);
	*y = ITEM_ROW_Y(idx / 2);
}

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

	char summer[40], scope[40], upd[40];
	snprintf(summer, sizeof(summer), "夏時間: %s", clock_summer_time() ? "ON" : "OFF");
	snprintf(scope, sizeof(scope), "ログ出力: %s", scopelog_enabled() ? "ON" : "OFF");
	snprintf(upd, sizeof(upd), "FW自動更新: %s", fwupdate_check_enabled() ? "ON" : "OFF");
	const struct { const char *label; bool on; } items[ITEM_N] = {
		{ "WiFi設定", false },
		{ "時刻合わせ", false },
		{ "FW更新", false },
		{ summer, clock_summer_time() != 0 },
		{ upd, fwupdate_check_enabled() },
		{ "タッチパネル調整", false },
		{ scope, scopelog_enabled() != 0 },
		{ "初期化", false },
		{ "About", false },
	};
	for (int i = 0; i < ITEM_N; i++) {
		int x, y;
		item_pos(i, &x, &y);
		draw_button(x, y, ITEM_COL_W, ITEM_H, items[i].label,
		            items[i].on ? C_OK_BG : C_BTN_BG,
		            items[i].on ? C_OK_BD : C_BTN_BD, C_BTN_TX,
		            &fonts::lgfxJapanGothicP_16);
	}
}

//	確認ダイアログは display.cpp の display_confirm() (SETUP と起動時
//	アップデートチェックで共有) を使う

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
			int ix, iy;
			item_pos(i, &ix, &iy);
			if (hit(tx, ty, ix, iy, ITEM_COL_W, ITEM_H)) {
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
		case 2:                     // FW更新 (SoftAP + ブラウザから手動アップロード)
			if (display_confirm(lcd, "OTAモードに入ります",
			            "受信を止めて WiFi を起動します",
			            "更新が成功すると再起動します", "開始")) {
				ap_mode_enter();
				ota_run(lcd);       // 成功時は再起動、キャンセルで戻る
				ap_mode_leave();
			}
			break;
		case 3:
			clock_set_summer_time(!clock_summer_time());
			break;
		case 4:                     // 起動時アップデートチェック ON/OFF
			fwupdate_set_check_enabled(!fwupdate_check_enabled());
			break;
		case 5:
			touchcal_run(lcd);      // タッチの四隅校正 (DSP は動いたまま)
			break;
		case 6:
			scopelog_set_enabled(!scopelog_enabled());
			break;
		case 7:
			if (display_confirm(lcd, "設定を初期化します",
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
