///////////////////////////////////////////////////////////////////////////
//
//	CW Decoder for ESP32 (air_monitor board)
//	バージョン: 2.0
//
//	CH32V006 版 v1.9 からの移植:
//	- Arduino + LovyanGFX (ST7789 240x320, 横向き 320x240)
//	- トーン検出を Goertzel から FFT ベースに変更
//	- 音声入力: GPIO35 (ADC1_CH7) を I2S DMA で 8kHz サンプリング
//	  ※GPIO27(ADC2)は I2S 内蔵ADC非対応のため使用しない
//	- 画面: 上2/3 デコード文字 (13列x6行, 24x24全角フォント/カタカナ対応)
//	        下1/3 左FFT / 右オシロ (生波形+エンベロープ+キー判定を同期表示)
//	- 操作: タッチパネル (XPT2046)
//	    ステータス行の US/JP バッジ    = 欧文/和文モード切替
//	    ステータス行の TONE 表示       = AUTO→600→700→800→900→1000 の順送り
//	      (AUTO = 600〜1000Hz の最強信号へ自動同調。デフォルト)
//	    FFT パネル内タップ             = タップ位置に最も近いトーンを手動選択
//	  BOOTボタン(GPIO0) でも操作可: 短押し=トーン切替 / 長押し=モード切替
//
//	原作: Hjalmar Skovholm Hansen OZ1JHM (GPL)
//	このソフトウェアは GNU General Public License (GPL) に基づき配布されています。
//
///////////////////////////////////////////////////////////////////////////
#include <Arduino.h>
#include <Wire.h>
#include "audio.h"
#include "dsp.h"
#include "decoder.h"
#include "display.h"

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

void setup()
{
	Serial.begin(115200);
	pinMode(PIN_BUTTON, INPUT_PULLUP);

#if TOUCH_DIAG
	delay(500);
	probe_i2c_touch();
#endif

	display_init();
	display_splash();

	decoder_init();
	decoder_set_emit(display_enqueue);

	audio_init();
	dsp_start();
}

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

void loop()
{
	uint32_t t0 = millis();
	poll_button();
	display_update();
	uint32_t dt = millis() - t0;
	if (dt < FRAME_MS) {
		delay(FRAME_MS - dt);
	}
}
