//
//	LovyanGFX board config (same wiring as air_monitor)
//	ESP32 + 240x320 LCD, HSPI: SCLK=14 MOSI=13 MISO=12 DC=2 CS=15 BL=21
//
//	LCDコントローラは ST7789 / ILI9341 を自動判定する (init_auto)。
//	MISO(GPIO12)が生きていれば ID4(0xD3) を読んで区別:
//	  ILI9341 -> 0x00,0x93,0x41 / それ以外は ST7789 とみなす。
//	MISO未結線の個体などで判定できない場合はビルドフラグで強制指定:
//	  -DPANEL_ST7789  または  -DPANEL_ILI9341
//
//	タッチの向き:
//	  LovyanGFX はタッチ座標を「パネルの offset_rotation + タッチの
//	  offset_rotation」で回す (Panel_Device::convertRawXY)。ILI9341 は
//	  パネル側に 2 を入れているので、タッチ側も同じ 2 のままだと
//	  ST7789 に対して 180°ずれる (時計の下段ボタンが押せない等)。
//	  そこで TOUCH_ROT_BASE から パネルの offset_rotation を引いた値を
//	  タッチへ与え、どちらのパネルでも合成後の回転が同じになるようにする。
//
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
	lgfx::Panel_ST7789  _panel_st7789;
	lgfx::Panel_ILI9341 _panel_ili9341;
	lgfx::Bus_SPI       _bus_instance;
	lgfx::Light_PWM     _light_instance;
	lgfx::Touch_XPT2046 _touch_instance;
	const char*         _panel_name = "ST7789";

	// パネルの回転オフセットを打ち消した後のタッチの向き (ST7789 実機基準)
	static constexpr uint8_t TOUCH_ROT_BASE = 2;

	// タッチの既定の補正値 (XPT2046 の生値。SETUP のタッチパネル調整を
	// していないときに使う)。x はタッチ素子の素の向きの短辺 (画面の縦方向)
	struct TouchRange { uint16_t x_min, x_max, y_min, y_max; };
	// ILI9341 基板の値 (従来の既定)
	static constexpr TouchRange TOUCH_ILI9341 = { 300, 3900, 3700, 200 };
	// ST7789 基板の値。実機で 2 回校正した四隅の生値の平均
	// (1226,2886 1439,44 4003,3071 4095,229 / 1140,2994 1406,43 3971,3147 4095,196)。
	// ILI9341 の値のままだと短辺方向が約 1000 ずれ、触った位置より下で反応した
	static constexpr TouchRange TOUCH_ST7789 = { 1300, 4040, 3030, 130 };

	// 使うパネルを決めて初期化する。パネルの offset_rotation を打ち消す値を
	// タッチへ入れてから init() するので、どちらのパネルでもタッチの向きは同じ。
	// 補正の既定値もパネルごとに入れ、setTouch() で LovyanGFX の補正行列を
	// 作り直す (行列はコンストラクタの setTouch() 時点の値で作られているため)
	bool use_panel(lgfx::Panel_LCD* p, const char* name, const TouchRange& r) {
		auto cfg = _touch_instance.config();
		cfg.offset_rotation =
			(uint8_t)((TOUCH_ROT_BASE + 4 - p->config().offset_rotation) & 3);
		cfg.x_min = r.x_min;
		cfg.x_max = r.x_max;
		cfg.y_min = r.y_min;
		cfg.y_max = r.y_max;
		_touch_instance.config(cfg);
		p->setTouch(&_touch_instance);
		_panel_name = name;
		setPanel(p);
		return init();
	}

	// 両コントローラ共通のパネル設定 (解像度は同一)。
	// offset_rotation は基準面の差を吸収する: ILI9341 は ST7789 に対して
	// 表示が180°回転しているため 2 を与え、表示コードとタッチ校正を共通化する。
	void config_panel(lgfx::Panel_LCD* p, uint8_t offset_rotation) {
		auto cfg = p->config();
		cfg.pin_cs = 15;
		cfg.pin_rst = -1;
		cfg.pin_busy = -1;
		cfg.memory_width = 240;
		cfg.memory_height = 320;
		cfg.panel_width = 240;
		cfg.panel_height = 320;
		cfg.offset_x = 0;
		cfg.offset_y = 0;
		cfg.offset_rotation = offset_rotation;
		cfg.readable = true;
		cfg.invert = false;
		p->config(cfg);
		p->setBus(&_bus_instance);
		p->setLight(&_light_instance);
		p->setTouch(&_touch_instance);
	}

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
		}
		{
			auto cfg = _light_instance.config();
			cfg.pin_bl = 21;
			cfg.invert = false;
			cfg.freq = 44100;
			cfg.pwm_channel = 7;
			_light_instance.config(cfg);
		}
		{
			// タッチ (XPT2046, LCDとは別SPI)
			// キャリブレーション値はボード個体差あり。既定値はパネルごとに
			// use_panel() で入れ直す (TOUCH_ST7789 / TOUCH_ILI9341)
			auto cfg = _touch_instance.config();
			cfg.x_min = TOUCH_ST7789.x_min;
			cfg.x_max = TOUCH_ST7789.x_max;
			cfg.y_min = TOUCH_ST7789.y_min;
			cfg.y_max = TOUCH_ST7789.y_max;
			cfg.pin_int = -1;   // INT配線の個体差に依存しないようSPIポーリングで検出
			cfg.bus_shared = false;
			// offset_rotation は init_auto() がパネルに合わせて入れ直す
			// (config_touch_rotation)。ここは ST7789 のときの値
			cfg.offset_rotation = TOUCH_ROT_BASE;
			cfg.spi_host = -1;  // ソフトSPI (CYD系はハードSPI割当だと動かない個体あり)
			cfg.freq = 1000000;
			cfg.pin_sclk = 25;
			cfg.pin_mosi = 32;
			cfg.pin_miso = 39;
			cfg.pin_cs = 33;
			_touch_instance.config(cfg);
		}
		config_panel(&_panel_st7789, 0);
		config_panel(&_panel_ili9341, 2);   // ILI9341は基準面が180°違う
		setPanel(&_panel_st7789);
	}

	// LCDを初期化し、ST7789/ILI9341 を自動判定する。
	// 強制指定するビルドフラグがあればそれを優先。
	bool init_auto(void) {
#if defined(PANEL_ILI9341)
		return use_panel(&_panel_ili9341, "ILI9341", TOUCH_ILI9341);
#elif defined(PANEL_ST7789)
		return use_panel(&_panel_st7789, "ST7789", TOUCH_ST7789);
#else
		// まず ST7789 として初期化 (SLPOUT/COLMOD/DISPON 等は両者共通)
		if (!use_panel(&_panel_st7789, "ST7789", TOUCH_ST7789)) {
			return false;
		}
		// ID4(0xD3) を読む。dummy_read_bits=1 は両パネル共通なので
		// ST7789 側から読んでも整合する。
		// ILI9341: byte列 0x00,0x93,0x41 -> (id>>8)&0xFFFF == 0x4193
		// 注意: ID読み出しに応答しない ILI9341 個体があり、その場合は
		// 判定できないので -DPANEL_ILI9341 で明示指定すること。
		uint32_t id = _panel_st7789.readCommand(0xD3, 0, 3);
		if (((id >> 8) & 0xFFFF) == 0x4193) {
			return use_panel(&_panel_ili9341, "ILI9341", TOUCH_ILI9341);
		}
		return true;
#endif
	}

	const char* panel_name(void) const { return _panel_name; }
};
