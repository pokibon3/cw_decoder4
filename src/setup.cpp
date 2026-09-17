//
//	セットアップ画面
//
//	┌ SETUP                               [戻る] ┐
//	│      [ 時刻合わせ ]                        │
//	│      [ ファームウェアアップデート ]        │
//	│      [ WiFi設定 ]                          │
//	│      [ WiFi初期化 ]                        │
//	│  バージョン / WiFi / 時刻同期              │
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
#define ITEM_H 30
#define ITEM1_Y 40
#define ITEM2_Y 74
#define ITEM3_Y 108
#define ITEM4_Y 142

#define INFO_Y 180

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

#define INFO_ROWS 3
#define INFO_VAL_X 116
#define INFO_PITCH 19

//	ステータス欄。毎秒呼ばれるが、値が変わった行だけ描き直す
//	(全部塗り直すとバージョン欄までちらついて読みにくいため)。
//	full=true でラベルを含めて全描画する。
static void draw_info(bool full)
{
	static String prev[INFO_ROWS];
	clock_tm_t tm;
	clock_break(clock_now(), &tm);

	static const char *LABEL[INFO_ROWS] = {
		"バージョン", "WiFi", "時刻同期",
	};
	String val[INFO_ROWS];
	char buf[72];

	// "Sep 16 2026 20:15:00" は長すぎて右端からはみ出すので年と秒を落とす
	// (FW_BUILD = "Mon DD YYYY HH:MM:SS")
	char d[8], t[6];
	memcpy(d, FW_BUILD, 6); d[6] = 0;
	memcpy(t, FW_BUILD + 12, 5); t[5] = 0;
	snprintf(buf, sizeof(buf), "v%s   %s %s", FW_VERSION, d, t);
	val[0] = buf;
	val[1] = netsync_has_wifi() ? netsync_ssid() : String("未設定 (WiFi設定で登録)");
	if (netsync_last_epoch()) {
		clock_tm_t s;
		clock_break(netsync_last_epoch(), &s);
		snprintf(buf, sizeof(buf), "NTP %02d/%02d %02d:%02d", s.mon, s.day, s.hour, s.min);
	} else {
		snprintf(buf, sizeof(buf), "未同期 (現在 %02d:%02d)", tm.hour, tm.min);
	}
	val[2] = buf;

	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	if (full) {
		lcd->fillRect(0, INFO_Y, 320, 240 - INFO_Y, C_BG);
		lcd->drawFastHLine(20, INFO_Y - 6, 280, lgfx::color565(40, 60, 85));
		lcd->setTextColor(C_LABEL, C_BG);
		for (int i = 0; i < INFO_ROWS; i++) {
			lcd->drawString(LABEL[i], 20, INFO_Y + i * INFO_PITCH);
			prev[i] = "";
		}
	}
	lcd->setTextColor(C_VALUE, C_BG);
	for (int i = 0; i < INFO_ROWS; i++) {
		if (val[i] == prev[i]) {
			continue;
		}
		prev[i] = val[i];
		int y = INFO_Y + i * INFO_PITCH;
		lcd->fillRect(INFO_VAL_X, y, 320 - INFO_VAL_X, INFO_PITCH, C_BG);
		lcd->setClipRect(INFO_VAL_X, y, 320 - INFO_VAL_X - 4, INFO_PITCH);
		lcd->drawString(val[i], INFO_VAL_X, y);
		lcd->clearClipRect();
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

	draw_button(ITEM_X, ITEM1_Y, ITEM_W, ITEM_H, "時刻合わせ",
	            C_BTN_BG, C_BTN_BD, C_BTN_TX, &fonts::lgfxJapanGothicP_16);
	draw_button(ITEM_X, ITEM2_Y, ITEM_W, ITEM_H, "ファームウェアアップデート",
	            C_BTN_BG, C_BTN_BD, C_BTN_TX, &fonts::lgfxJapanGothicP_16);
	draw_button(ITEM_X, ITEM3_Y, ITEM_W, ITEM_H, "WiFi設定 (NTP時刻同期用)",
	            C_BTN_BG, C_BTN_BD, C_BTN_TX, &fonts::lgfxJapanGothicP_16);
	draw_button(ITEM_X, ITEM4_Y, ITEM_W, ITEM_H, "WiFi初期化",
	            C_BTN_BG, C_BTN_BD, C_BTN_TX, &fonts::lgfxJapanGothicP_16);

	draw_info(true);
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
	uint32_t t_info = millis();
	for (;;) {
		if (millis() - t_info > 1000) {         // 空きメモリ等を更新
			t_info = millis();
			draw_info(false);
		}
		if (!lcd->getTouch(&tx, &ty)) {
			delay(10);
			continue;
		}

		if (hit(tx, ty, BACK_X, BACK_Y, BACK_W, BACK_H)) {
			wait_release();
			return;
		}
		if (hit(tx, ty, ITEM_X, ITEM1_Y, ITEM_W, ITEM_H)) {
			wait_release();
			timeset_run(lcd);
			draw_screen();
		} else if (hit(tx, ty, ITEM_X, ITEM2_Y, ITEM_W, ITEM_H)) {
			wait_release();
			if (confirm("OTAモードに入ります",
			            "受信を止めて WiFi を起動します",
			            "更新が成功すると再起動します", "開始")) {
				ap_mode_enter();
				ota_run(lcd);       // 成功時は再起動、キャンセルで戻る
				ap_mode_leave();
			}
			draw_screen();
		} else if (hit(tx, ty, ITEM_X, ITEM3_Y, ITEM_W, ITEM_H)) {
			wait_release();
			ap_mode_enter();
			wifi_setup_run(lcd);
			ap_mode_leave();
			draw_screen();
		} else if (hit(tx, ty, ITEM_X, ITEM4_Y, ITEM_W, ITEM_H)) {
			wait_release();
			if (confirm("WiFi設定を初期化します",
			            "保存済みの SSID / パスワードを消し",
			            "NTP 時刻同期を止めます", "初期化")) {
				netsync_clear_wifi();
			}
			draw_screen();
		} else {
			wait_release();
		}
	}
}
