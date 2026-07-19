//
//	LovyanGFX board config (same wiring as air_monitor)
//	ESP32 + ST7789 240x320, HSPI: SCLK=14 MOSI=13 MISO=12 DC=2 CS=15 BL=21
//
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
	lgfx::Panel_ST7789 _panel_instance;
	lgfx::Bus_SPI _bus_instance;
	lgfx::Light_PWM _light_instance;
	lgfx::Touch_XPT2046 _touch_instance;

public:
	LGFX(void) {
		{
			auto cfg = _bus_instance.config();
			cfg.spi_host = SPI2_HOST;
			cfg.spi_mode = 3;
			cfg.freq_write = 40000000;
			cfg.freq_read = 16000000;
			cfg.spi_3wire = false;
			cfg.use_lock = true;
			cfg.dma_channel = SPI_DMA_CH_AUTO;
			cfg.pin_sclk = 14;
			cfg.pin_mosi = 13;
			cfg.pin_miso = 12;
			cfg.pin_dc = 2;
			_bus_instance.config(cfg);
			_panel_instance.setBus(&_bus_instance);
		}
		{
			auto cfg = _panel_instance.config();
			cfg.pin_cs = 15;
			cfg.pin_rst = -1;
			cfg.pin_busy = -1;
			cfg.memory_width = 240;
			cfg.memory_height = 320;
			cfg.panel_width = 240;
			cfg.panel_height = 320;
			cfg.offset_x = 0;
			cfg.offset_y = 0;
			cfg.offset_rotation = 0;
			cfg.invert = false;
			_panel_instance.config(cfg);
		}
		{
			auto cfg = _light_instance.config();
			cfg.pin_bl = 21;
			cfg.invert = false;
			cfg.freq = 44100;
			cfg.pwm_channel = 7;
			_light_instance.config(cfg);
			_panel_instance.setLight(&_light_instance);
		}
		{
			// タッチ (XPT2046, LCDとは別SPI)
			// キャリブレーション値はボード個体差あり: ズレる場合は
			// x_min/x_max/y_min/y_max を調整すること
			auto cfg = _touch_instance.config();
			cfg.x_min = 300;
			cfg.x_max = 3900;
			cfg.y_min = 3700;
			cfg.y_max = 200;
			cfg.pin_int = -1;   // INT配線の個体差に依存しないようSPIポーリングで検出
			cfg.bus_shared = false;
			cfg.offset_rotation = 2;  // 実機で座標が180度ズレるため補正
			cfg.spi_host = -1;  // ソフトSPI (CYD系はハードSPI割当だと動かない個体あり)
			cfg.freq = 1000000;
			cfg.pin_sclk = 25;
			cfg.pin_mosi = 32;
			cfg.pin_miso = 39;
			cfg.pin_cs = 33;
			_touch_instance.config(cfg);
			_panel_instance.setTouch(&_touch_instance);
		}
		setPanel(&_panel_instance);
	}
};
