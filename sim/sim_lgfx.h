//
//	PC シミュレータ用の LGFX (src/lgfx_config.h が CWDEC_SIM のときに読む)
//
//	LCD の代わりに SDL のウィンドウへ描く。実機と同じく 240x320 縦置きの
//	パネルを setRotation(1) で 320x240 横にして使うので、描画コードも
//	タッチの向きの計算もファームウェアと同じ経路を通る。
//	ウィンドウはフレームごと回して横長に見せる (FRAME_ROTATION)。
//	マウスの左ボタンがタッチ、sim_main.cpp が台本からのタップも差し込む。
//
//	CWDEC_SIM_HD (cwsim-hd、画面写真用) のときは、パネルを 2 倍 (480x640) にして
//	ファームの描画命令の座標をすべて 2 倍にして描く (HDGfx)。文字だけは
//	元のビットマップフォントではなく、Mac のフォント (CoreText) で
//	2 倍の大きさに描き直す (sim_hdfont.cpp)。文字の位置と送り幅は元のフォントの
//	ものを使うので、配置は実機と同じで、字形だけがなめらかになる
//
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <vector>

//	台本 (--script) のタップを差し込めるパネル。座標はパネル素の向き
struct Panel_sim : public lgfx::Panel_sdl {
	volatile bool inject = false;
	volatile int16_t inject_x = 0, inject_y = 0;

	uint_fast8_t getTouchRaw(lgfx::touch_point_t *tp, uint_fast8_t count) override {
		if (inject) {
			tp->x = inject_x;
			tp->y = inject_y;
			tp->size = 1;
			tp->id = 0;
			return 1;
		}
		return lgfx::Panel_sdl::getTouchRaw(tp, count);
	}
};

#if defined(CWDEC_SIM_HD)
static constexpr int SIM_K = 2;             // 描画の倍率
#else
static constexpr int SIM_K = 1;
#endif

//	SDL のパネルを持つ LGFX の本体。倍率 SIM_K のぶん大きいパネルにする
class LGFX_SimDevice : public lgfx::LGFX_Device {
	Panel_sim _panel_instance;
	// 補正 (touchcal.cpp) が設定値を読むので、生値 = 画素座標の範囲を持たせる
	lgfx::Touch_sdl _touch_instance;

protected:
	bool init_impl(bool use_reset, bool use_clear) {
		return lgfx::LGFX_Device::init_impl(false, use_clear);
	}

public:
	static constexpr int SCALE = (SIM_K == 1) ? 2 : 1;     // ウィンドウの拡大率
	static constexpr int FRAME_ROTATION = 3;    // 論理の左上はパネル素の右上に来るので 270° 回す

	LGFX_SimDevice(void) {
		auto cfg = _panel_instance.config();
		cfg.memory_width = 240 * SIM_K;
		cfg.memory_height = 320 * SIM_K;
		cfg.panel_width = 240 * SIM_K;
		cfg.panel_height = 320 * SIM_K;
		_panel_instance.config(cfg);
		{
			auto tc = _touch_instance.config();
			tc.x_min = 0;
			tc.x_max = 240 * SIM_K - 1;
			tc.y_min = 0;
			tc.y_max = 320 * SIM_K - 1;
			tc.offset_rotation = 0;
			_touch_instance.config(tc);
		}
		_panel_instance.setTouch(&_touch_instance);
		_panel_instance.setScaling(SCALE, SCALE);
		_panel_instance.setFrameRotation(FRAME_ROTATION);
		_panel_instance.setWindowTitle("CW Decoder 4 Simulator");
		setPanel(&_panel_instance);
	}

	bool init_auto(void) { return init(); }
	// About 画面に出る名前。画面写真を実機と同じ表記にするため ST7789 を名乗る
	const char *panel_name(void) const { return "ST7789"; }
	Panel_sim *sim_panel(void) { return &_panel_instance; }
};

#if !defined(CWDEC_SIM_HD)

using LGFX = LGFX_SimDevice;

#else

namespace simhd {
//	元のフォントに対応する「字形だけ CoreText で描く」フォント (sim_hdfont.cpp)
const lgfx::IFont *hd_font(const lgfx::IFont *orig);
}

//	ファームの描画命令を受けて、座標を K 倍して本体 (B) へ渡す。
//	B は protected で継承するので、ここに無い命令をファームが使うと
//	コンパイルエラーになる (1 倍の座標のまま描かれる取りこぼしが起きない)
template <class B>
class HDGfx : protected B {
	template <class> friend class HDGfx;

public:
	static constexpr int K = SIM_K;
	using B::B;

	lgfx::LovyanGFX *base_gfx(void) { return static_cast<B *>(this); }

protected:
	// 文字の大きさの初期値 (ファームが setTextSize を呼ばないときの 1 倍) も K 倍にする
	void init_k(void) { B::setTextSize((float)K); }

public:

	int32_t width(void) const { return B::width() / K; }
	int32_t height(void) const { return B::height() / K; }

	void startWrite(void) { B::startWrite(); }
	void endWrite(void) { B::endWrite(); }
	void setColorDepth(int bits) { B::setColorDepth(bits); }
	void setRotation(uint_fast8_t r) { B::setRotation(r); }
	uint_fast8_t getRotation(void) const { return B::getRotation(); }

	template <typename T> void fillScreen(const T &c) { B::fillScreen(c); }
	template <typename T> void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, const T &c) {
		B::fillRect(x * K, y * K, w * K, h * K, c);
	}
	template <typename T> void drawFastHLine(int32_t x, int32_t y, int32_t w, const T &c) {
		B::fillRect(x * K, y * K, w * K, K, c);
	}
	template <typename T> void drawFastVLine(int32_t x, int32_t y, int32_t h, const T &c) {
		B::fillRect(x * K, y * K, K, h * K, c);
	}
	//	1 画素の線は K 画素の太さで描く (最近傍で拡大したのと同じ太さ)
	template <typename T> void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, const T &c) {
		for (int i = 0; i < K; i++) B::drawRect(x * K + i, y * K + i, w * K - 2 * i, h * K - 2 * i, c);
	}
	template <typename T> void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const T &c) {
		for (int i = 0; i < K; i++) {
			B::drawRoundRect(x * K + i, y * K + i, w * K - 2 * i, h * K - 2 * i, r * K - i, c);
		}
	}
	template <typename T> void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const T &c) {
		B::fillRoundRect(x * K, y * K, w * K, h * K, r * K, c);
	}
	//	点の座標は画素の中心 (x + 0.5) を K 倍した位置へ
	static int32_t mid(int32_t v) { return v * K + K / 2; }
	template <typename T> void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
	                                        int32_t x2, int32_t y2, const T &c) {
		B::fillTriangle(mid(x0), mid(y0), mid(x1), mid(y1), mid(x2), mid(y2), c);
	}
	template <typename T> void fillCircle(int32_t x, int32_t y, int32_t r, const T &c) {
		B::fillCircle(mid(x), mid(y), r * K, c);
	}
	template <typename T> void drawCircle(int32_t x, int32_t y, int32_t r, const T &c) {
		for (int i = 0; i < K; i++) B::drawCircle(mid(x), mid(y), r * K - i, c);
	}
	template <typename T> void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const T &c) {
		B::drawWideLine(mid(x0), mid(y0), mid(x1), mid(y1), K / 2.0f, c);
	}
	uint16_t readPixel(int32_t x, int32_t y) { return B::readPixel(x * K, y * K); }
	void qrcode(const char *s, int32_t x, int32_t y, int32_t w, uint8_t ver) {
		B::qrcode(s, x * K, y * K, w * K, ver);
	}

	void setClipRect(int32_t x, int32_t y, int32_t w, int32_t h) { B::setClipRect(x * K, y * K, w * K, h * K); }
	void clearClipRect(void) { B::clearClipRect(); }

	//	文字: 位置と送り幅は元のフォントを K 倍した値で LovyanGFX が決め、
	//	字形だけ simhd のフォントが描く。寸法を返すものは 1 倍に戻す
	void setFont(const lgfx::IFont *f) { B::setFont(simhd::hd_font(f)); }
	void setTextSize(float s) { B::setTextSize(s * K); }
	template <typename T> void setTextColor(T fg) { B::setTextColor(fg); }
	template <typename T1, typename T2> void setTextColor(T1 fg, T2 bg) { B::setTextColor(fg, bg); }
	void setTextDatum(lgfx::textdatum_t d) { B::setTextDatum(d); }
	void setTextDatum(uint8_t d) { B::setTextDatum(d); }
	void setTextWrap(bool w) { B::setTextWrap(w); }
	void setCursor(int32_t x, int32_t y) { B::setCursor(x * K, y * K); }
	size_t print(const char *s) { return B::print(s); }
	size_t drawString(const char *s, int32_t x, int32_t y) { return B::drawString(s, x * K, y * K); }
	int32_t textWidth(const char *s) { return B::textWidth(s) / K; }
	int32_t fontHeight(void) { return B::fontHeight() / K; }

	// スプライト
	void setPivot(float x, float y) { B::setPivot(x * K, y * K); }
	template <typename T> void fillSprite(const T &c) { B::fillSprite(c); }
	void pushSprite(int32_t x, int32_t y) { B::pushSprite(x * K, y * K); }
	template <class D> void pushSprite(D *dst, int32_t x, int32_t y) {
		B::pushSprite(dst->base_gfx(), x * K, y * K);
	}
	template <class D> void pushRotateZoom(D *dst, float x, float y, float angle, float zx, float zy) {
		B::pushRotateZoom(dst->base_gfx(), x * K, y * K, angle, zx, zy);
	}
};

//	LCD (display.cpp の lcd)
class LGFX : public HDGfx<LGFX_SimDevice> {
public:
	LGFX(void) { init_k(); }
	bool init_auto(void) {
		bool ok = LGFX_SimDevice::init_auto();
		init_k();
		return ok;
	}
	const char *panel_name(void) const { return LGFX_SimDevice::panel_name(); }
	Panel_sim *sim_panel(void) { return LGFX_SimDevice::sim_panel(); }
	void setBrightness(uint8_t b) { LGFX_SimDevice::setBrightness(b); }

	// タッチ: 生値はそのまま、座標は 1 倍に戻す
	lgfx::ITouch *touch(void) const { return LGFX_SimDevice::touch(); }
	lgfx::Panel_Device *panel(void) const { return LGFX_SimDevice::panel(); }
	void setTouchCalibrate(uint16_t *p) { LGFX_SimDevice::setTouchCalibrate(p); }
	uint_fast8_t getTouchRaw(lgfx::touch_point_t *tp, uint_fast8_t n = 1) {
		return LGFX_SimDevice::getTouchRaw(tp, n);
	}
	template <typename T> uint_fast8_t getTouch(T *x, T *y) {
		T tx, ty;
		uint_fast8_t n = LGFX_SimDevice::getTouch(&tx, &ty);
		if (n) {
			*x = tx / K;
			*y = ty / K;
		}
		return n;
	}

	// 画面写真 (sim_main.cpp)。2 倍のまま PNG にする
	void *sim_png(size_t *len) {
		return LGFX_SimDevice::createPng(len, 0, 0, LGFX_SimDevice::width(), LGFX_SimDevice::height());
	}
};

//	スプライト。ファームは共有バッファを setBuffer で割り当てるが、
//	K 倍の大きさが要るので各スプライトが自前のバッファを持つ
class SimSprite : public HDGfx<lgfx::LGFX_Sprite> {
	std::vector<uint8_t> buf_;

public:
	SimSprite(void) : HDGfx<lgfx::LGFX_Sprite>() { init_k(); }
	SimSprite(LGFX *parent) : HDGfx<lgfx::LGFX_Sprite>(parent->base_gfx()) { init_k(); }

	void setBuffer(void *buffer, int32_t w, int32_t h, uint8_t bpp = 0) {
		(void)buffer;
		if (bpp) lgfx::LGFX_Sprite::setColorDepth(bpp);
		const int bytes = (lgfx::LGFX_Sprite::getColorDepth() & lgfx::color_depth_t::bit_mask) / 8;
		buf_.assign((size_t)w * K * h * K * (bytes ? bytes : 1), 0);
		lgfx::LGFX_Sprite::setBuffer(buf_.data(), w * K, h * K);
	}
};

#define LGFX_Sprite SimSprite

#endif  // CWDEC_SIM_HD
