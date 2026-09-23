///////////////////////////////////////////////////////////////////////////
//
//	CW Decoder for ESP32 (air_monitor board)
//	バージョン: version.h (FW_VERSION)
//
//	CH32V006 版 v1.9 からの移植:
//	- Arduino + LovyanGFX (ST7789 240x320, 横向き 320x240)
//	- トーン検出を Goertzel から FFT ベースに変更
//	- 音声入力: GPIO35 (ADC1_CH7) をADC continuous DMAで32kHz取得、4点平均で8kHz化
//	  ※continuous DMAはADC1のみ使用
//	- 画面: 上2/3 デコード文字 (13列x6行, 24x24全角フォント/カタカナ対応)
//	        下1/3 左FFT / 右オシロ (生波形+エンベロープ+キー判定を同期表示)
//	- 操作: タッチパネル (XPT2046)
//	    ステータス行の US/JP バッジ    = 欧文/和文モード切替
//	    ステータス行の TONE ボタン     = AUTO→600→700→800→900→1000 の順送り
//	      (AUTO = 500〜1000Hz の最強信号へ自動同調。デフォルト)
//	    FFT パネル内タップ             = タップ位置に最も近いトーンを手動選択
//	    ステータス行の SETUP ボタン    = SETUP 画面へ
//	    文字エリア中央付近タップ       = 時計画面へ (時計中央タップで戻る)
//	  BOOTボタン(GPIO0) でも操作可: 短押し=トーン切替 / 長押し=モード切替
//	  BOOTボタンを押したまま電源投入 = タッチ調整を消して既定値へ戻す
//	  (SETUP のタッチ調整でズレて操作不能になったときの逃げ道)
//
//	画面遷移 (デコーダの動作に影響を与えないことを最優先):
//	  デコーダ画面 (既定)
//	    ├ SETUP ...... 時刻合わせ / ファームウェアアップデート (OTA) /
//	    │              WiFi設定 / WiFi初期化 / スコープログ / タッチ調整。
//	    │              OTA・WiFi設定は受信を止めて AP を立て、
//	    │              [キャンセル] または保存/更新完了で戻る
//	    └ 時計画面 ... 世界時計 (下段の国コードを押すとその国の時刻になる。
//	                   パタパタ部分をタップするとデコーダ画面へ戻る)。
//	                   DSP/デコーダは裏で動き続け、受信文字は溜まる。
//	                   WiFi による NTP 同期はこの画面でしか行わず、
//	                   同期中は DSP を一時停止して無線ノイズを遮断する
//	  デコーダ画面では WiFi を一切起動しない。
//
//	原作: Hjalmar Skovholm Hansen OZ1JHM (GPL)
//	このソフトウェアは GNU General Public License (GPL) に基づき配布されています。
//
///////////////////////////////////////////////////////////////////////////
#include <Arduino.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include "audio.h"
#include "dsp.h"
#include "decoder.h"
#include "display.h"
#include "version.h"
#include "clock.h"
#include "setup.h"
#include "netsync.h"
#include "fwupdate.h"
#include "scopelog.h"
#include "touchcal.h"

const char FW_BUILD[] = __DATE__ " " __TIME__;

// タッチ診断: 起動時にI2Cスキャン(静電容量式コントローラの検出用)を行い、
// タッチ検出時は画面に赤ドット + シリアルへ座標を出力する
#define TOUCH_DIAG 0

#if TOUCH_DIAG
// 静電容量式タッチ(CST820=0x15, FT6x36=0x38, GT911=0x5D/0x14 等)が
// 載っていればここで見つかる。display_init() より前に呼ぶこと。
static void probe_i2c_touch(void)
{
	Serial.println("[diag] I2C scan SDA=33 SCL=32 ...");
	Wire.begin(33, 32);
	int found = 0;
	for (uint8_t addr = 1; addr < 127; addr++) {
		Wire.beginTransmission(addr);
		if (Wire.endTransmission() == 0) {
			Serial.printf("[diag]   found device at 0x%02X\n", addr);
			found++;
		}
	}
	if (found == 0) {
		Serial.println("[diag]   no I2C device (capacitive touch not on 33/32)");
	}
	Wire.end();
}
#endif

#define PIN_BUTTON 0
#define LONG_PRESS_MS 800
#define FRAME_MS 33

//==================================================================
//	画面遷移
//==================================================================
enum { SCR_DECODER = 0, SCR_CLOCK };
static uint8_t screen = SCR_DECODER;
static uint8_t clock_requested = 0;     // デコーダ画面の中央タップで立つ
static uint8_t setup_requested = 0;     // ステータス行の SETUP ボタンで立つ

// 時計画面: パタパタのカード部分をタップするとデコーダ画面へ戻る
// (下段は世界時計のボタンなので、そこは clock_zone_touch が処理する)
#define CLK_BACK_Y0 36
#define CLK_BACK_Y1 162

static bool hit(int tx, int ty, int x, int y, int w, int h)
{
	return (tx >= x && tx < x + w && ty >= y && ty < y + h);
}

static void wait_release(void)
{
	int32_t tx, ty;
	while (display_lcd()->getTouch(&tx, &ty)) {
		delay(10);
	}
}

static void on_center_tap(void)
{
	clock_requested = 1;
}

static void on_setup_tap(void)
{
	setup_requested = 1;
}

//	SETUP 画面 (デコーダ画面から開く)。DSP は動いたままで、受信文字は溜まる
static void enter_setup(void)
{
	display_set_visible(0);
	setup_run(display_lcd());
	display_redraw();
	wait_release();
}

static void enter_clock(void)
{
	screen = SCR_CLOCK;
	display_set_visible(0);         // 受信文字は溜め続ける
	clock_alloc();                  // 共有バッファを時計側へ割り付け直す
	clock_redraw();
	wait_release();
}

static void leave_clock(void)
{
	screen = SCR_DECODER;
	display_redraw();               // 溜まった文字を含めて描き直す
	wait_release();
}

//	時計画面の1周期: 時計更新 / 期限が来ていれば NTP 同期 / タッチ処理
static void clock_screen_loop(void)
{
	int32_t tx, ty;

	clock_update();
	netsync_poll();                 // 同期中は DSP を止める (netsync.cpp)
	display_update();               // 非表示中は文字の取り込みのみ

	if (display_lcd()->getTouch(&tx, &ty)) {
		if (ty >= CLK_BACK_Y0 && ty < CLK_BACK_Y1) {
			leave_clock();          // パタパタ部分 = デコーダ画面へ戻る
			return;
		}
		// 下段の世界時計: 押した国の時刻へ (カードはパタパタで切り替わる)
		clock_zone_touch(tx, ty);
		wait_release();
	}
	delay(20);
}

//==================================================================
//	BOOT ボタン (デコーダ画面のみ)
//==================================================================
static void poll_button(void)
{
	static uint8_t pressed = 0;
	static uint32_t press_ms = 0;
	static uint8_t long_done = 0;

	uint8_t now = (digitalRead(PIN_BUTTON) == LOW);
	if (now && !pressed) {
		pressed = 1;
		long_done = 0;
		press_ms = millis();
	} else if (now && pressed && !long_done) {
		if ((millis() - press_ms) >= LONG_PRESS_MS) {
			decoder_toggle_mode();
			long_done = 1;
		}
	} else if (!now && pressed) {
		pressed = 0;
		if (!long_done && (millis() - press_ms) >= 30) {
			dsp_set_tone((uint8_t)((dsp_tone_index() + 1) % (DSP_TONE_COUNT + 1)));
		}
	}
}

//	タッチ調整でズレて画面が操作できなくなったときの逃げ道。
//	BOOT ボタンを押したまま電源を入れ、そのまま押し続けると校正値を消す
#define ESCAPE_HOLD_MS 2000

static void touchcal_escape(void)
{
	if (digitalRead(PIN_BUTTON) != LOW) {
		return;
	}
	LGFX *lcd = display_lcd();
	lcd->fillScreen(TFT_BLACK);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->setTextColor(TFT_WHITE, TFT_BLACK);
	lcd->drawString("そのまま押し続けると", 160, 100);
	lcd->drawString("タッチ調整を消去します", 160, 124);

	uint32_t t0 = millis();
	while ((millis() - t0) < ESCAPE_HOLD_MS) {
		if (digitalRead(PIN_BUTTON) != LOW) {   // 離したら何もしない
			lcd->setTextDatum(lgfx::textdatum_t::top_left);
			return;
		}
		delay(20);
	}
	touchcal_reset(lcd);
	lcd->fillScreen(TFT_BLACK);
	lcd->drawString("タッチ調整を消去しました", 160, 112);
	lcd->drawString("(既定値に戻しました)", 160, 136);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
	Serial.println("[touch] calibration cleared by BOOT button");
	while (digitalRead(PIN_BUTTON) == LOW) {
		delay(20);
	}
	delay(600);
}

void setup()
{
	Serial.setTxBufferSize(4096);   // スコープログ用 (送出側は非ブロッキング)
	Serial.begin(115200);
	pinMode(PIN_BUTTON, INPUT_PULLUP);

#if TOUCH_DIAG
	delay(500);
	probe_i2c_touch();
#endif

	display_init();
	touchcal_escape();              // BOOT 押しっぱなし起動 = 校正値の消去
	{
		const esp_partition_t *run = esp_ota_get_running_partition();
		Serial.printf("[boot] v%s build %s  running=%s @0x%06X  heap=%u\n",
		              FW_VERSION, FW_BUILD, run ? run->label : "?",
		              run ? (unsigned)run->address : 0, (unsigned)ESP.getFreeHeap());
	}
	display_splash();
	display_set_center_tap(on_center_tap);
	display_set_setup_tap(on_setup_tap);

	// 時計 / NTP 設定の読み込み (WiFi はここでは起動しない)
	netsync_init();
	clock_init(display_lcd());      // ここで時刻がビルド日時に戻るので同期はこの後
	clock_alloc();                  // 字面の実測 (初回のみ)

	// 起動時のオンラインアップデートチェック (自動更新 ON + WiFi登録済みのとき)。
	// 状況はスプラッシュの最下段に出る。NTP 同期も一緒に済ませる。
	// dsp_start() より前に済ませること — ADC 連続 DMA を動かしたまま
	// WiFi を起動すると WDT リセットになる
	fwupdate_init();
	fwupdate_check_on_boot(display_lcd());
	display_redraw();               // ここで初めてスプラッシュからデコーダ画面へ

	decoder_init();
	decoder_set_emit(display_enqueue);

	audio_init();
	dsp_start();

	// スコープログは既定で ON (SETUP で切れる)。送出は表示ループからの
	// 非ブロッキング処理で、受け手がいなければそのまま捨てられる
	scopelog_set_enabled(1);
}

void loop()
{
	if (screen == SCR_CLOCK) {
		clock_screen_loop();
		return;
	}

	uint32_t t0 = millis();
	poll_button();
	display_update();
	if (setup_requested) {
		setup_requested = 0;
		enter_setup();
		return;
	}
	if (clock_requested) {
		clock_requested = 0;
		enter_clock();
		return;
	}
	uint32_t dt = millis() - t0;
	if (dt < FRAME_MS) {
		delay(FRAME_MS - dt);
	}
}
