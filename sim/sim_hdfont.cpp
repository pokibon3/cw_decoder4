//
//	画面写真用 (cwsim-hd): 字形だけを Mac のフォントで描き直すフォント
//
//	ファームのフォントはすべて 2 値のビットマップで、2 倍に描くと段々が出る。
//	ここでは LovyanGFX のフォント (IFont) を包み、寸法 (送り幅・高さ・ベースライン) は
//	元のフォントのものをそのまま返して、字形だけを CoreText で描く。描く書体は
//	ビットマップの元になった書体そのもの (fetch_fonts.sh) なので、字の形は本体と同じになる。
//	文字の置き場所は LovyanGFX が元のフォントの寸法で決めるので、配置は実機と同じになる。
//
//	1 文字ごとに、元のフォントで描いたときのインクの範囲 (外接矩形) を調べ、
//	CoreText の字形のインクの中心をそこへ合わせる。字の大きさはフォントごとに
//	基準の字 ("H" など) の高さが元と同じになるよう決める。
//	背景を塗る指定のときは、元のフォントで描いて背景を塗らせたあと、
//	字形の部分だけを背景色で描き消してから重ねる (塗る範囲を元と同じにするため)。
//
#include <LovyanGFX.hpp>
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <string>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace simhd {

namespace {

struct Ink {
	bool any = false;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // 外接矩形 (x1, y1 は含まない)
};

//	CoreText で描いた 1 文字 (アルファのみ)
struct Glyph {
	int w = 0, h = 0;
	std::vector<uint8_t> a;
	Ink ink;
};

//	元のフォントのインクを調べるための作業用スプライト (1 文字ぶん)
#define SCRATCH_W 400
#define SCRATCH_H 300
#define SCRATCH_X 100
#define SCRATCH_Y 100

std::recursive_mutex lock;                  // 描画はファームと DSP 以外の 1 スレッドだが念のため

lgfx::LGFX_Sprite *scratch(void)
{
	static lgfx::LGFX_Sprite *s = nullptr;
	if (!s) {
		s = new lgfx::LGFX_Sprite();
		s->setColorDepth(8);
		s->createSprite(SCRATCH_W, SCRATCH_H);
	}
	return s;
}

Ink find_ink(const uint8_t *a, int w, int h, int stride, uint8_t thr)
{
	Ink k;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (a[y * stride + x] <= thr) continue;
			if (!k.any) {
				k.any = true;
				k.x0 = x; k.x1 = x + 1; k.y0 = y; k.y1 = y + 1;
			} else {
				if (x < k.x0) k.x0 = x;
				if (x >= k.x1) k.x1 = x + 1;
				if (y < k.y0) k.y0 = y;
				if (y >= k.y1) k.y1 = y + 1;
			}
		}
	}
	return k;
}

//	元のフォントで 1 文字描いたときのインク (描く位置からの相対) と送り幅
Ink orig_ink(const lgfx::IFont *f, uint16_t c, const lgfx::TextStyle *st,
             const lgfx::FontMetrics *m, size_t *adv)
{
	auto *s = scratch();
	s->fillSprite(0);
	lgfx::TextStyle t = *st;
	t.fore_rgb888 = t.back_rgb888 = 0xFFFFFF;    // 背景は塗らない
	lgfx::FontMetrics mm = *m;
	int32_t fx = 0;
	*adv = f->drawChar(s, SCRATCH_X, SCRATCH_Y, c, &t, &mm, fx);
	std::vector<uint8_t> a(SCRATCH_W * SCRATCH_H);
	for (int y = 0; y < SCRATCH_H; y++) {
		for (int x = 0; x < SCRATCH_W; x++) {
			a[y * SCRATCH_W + x] = s->readPixel(x, y) ? 255 : 0;
		}
	}
	Ink k = find_ink(a.data(), SCRATCH_W, SCRATCH_H, SCRATCH_W, 0);
	k.x0 -= SCRATCH_X; k.x1 -= SCRATCH_X;
	k.y0 -= SCRATCH_Y; k.y1 -= SCRATCH_Y;
	return k;
}

//	フォントの指定: "file:名前" は fetch_fonts.sh が置いた build/fonts/ のファイル、
//	それ以外は Mac に入っているフォントの名前
std::string font_dir(void)
{
	char path[4096];
	uint32_t n = sizeof(path);
	if (_NSGetExecutablePath(path, &n) != 0) return "build/fonts";
	std::string p(path);
	size_t sl = p.find_last_of('/');
	return (sl == std::string::npos ? std::string(".") : p.substr(0, sl)) + "/fonts";
}

CTFontRef make_font(const char *spec, float size)
{
	if (strncmp(spec, "file:", 5) != 0) {
		CFStringRef name = CFStringCreateWithCString(nullptr, spec, kCFStringEncodingUTF8);
		CTFontRef f = CTFontCreateWithName(name, size, nullptr);
		CFRelease(name);
		return f;
	}
	static std::map<std::string, CGFontRef> files;
	auto it = files.find(spec);
	if (it == files.end()) {
		std::string path = font_dir() + "/" + (spec + 5);
		CGDataProviderRef dp = CGDataProviderCreateWithFilename(path.c_str());
		CGFontRef cg = dp ? CGFontCreateWithDataProvider(dp) : nullptr;
		if (dp) CGDataProviderRelease(dp);
		if (!cg) {
			fprintf(stderr, "[sim] フォント %s がありません (sh fetch_fonts.sh で取ってくる)。"
			                "代わりに Mac のフォントで描きます\n", path.c_str());
		}
		it = files.emplace(spec, cg).first;
	}
	if (!it->second) {
		CFStringRef name = CFStringCreateWithCString(nullptr, "HiraginoSans-W3", kCFStringEncodingUTF8);
		CTFontRef f = CTFontCreateWithName(name, size, nullptr);
		CFRelease(name);
		return f;
	}
	return CTFontCreateWithGraphicsFont(it->second, size, nullptr, nullptr);
}

Glyph ct_glyph(const char *font_spec, float size, uint16_t c)
{
	Glyph g;
	CTFontRef font = make_font(font_spec, size);
	UniChar uc = c;
	CFStringRef str = CFStringCreateWithCharacters(nullptr, &uc, 1);
	CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
	CFTypeRef vals[] = { font, kCFBooleanTrue };
	CFDictionaryRef attr = CFDictionaryCreate(nullptr, (const void **)keys, (const void **)vals, 2,
	                                          &kCFTypeDictionaryKeyCallBacks,
	                                          &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attr);
	CTLineRef line = CTLineCreateWithAttributedString(as);

	const int pad = (int)(size * 0.5f) + 4;
	g.w = (int)(size * 2.0f) + pad * 2;
	g.h = (int)(size * 2.0f) + pad * 2;
	g.a.assign((size_t)g.w * g.h, 0);
	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	CGContextRef ctx = CGBitmapContextCreate(g.a.data(), g.w, g.h, 8, g.w, gray, kCGImageAlphaNone);
	CGContextSetShouldAntialias(ctx, true);
	CGContextSetShouldSmoothFonts(ctx, false);
	CGContextSetGrayFillColor(ctx, 1.0, 1.0);
	CGContextSetTextPosition(ctx, pad, pad + size * 0.5f);
	CTLineDraw(line, ctx);
	CGContextRelease(ctx);
	CGColorSpaceRelease(gray);

	CFRelease(line);
	CFRelease(as);
	CFRelease(attr);
	CFRelease(str);
	CFRelease(font);
	g.ink = find_ink(g.a.data(), g.w, g.h, g.w, 40);
	return g;
}

struct HDFont : public lgfx::IFont {
	const lgfx::IFont *orig;
	const char *ct_name;
	const char *ref;                        // 大きさの基準にする字

	mutable std::map<std::tuple<uint16_t, float, float>, std::tuple<Ink, size_t>> ink_cache;
	mutable std::map<float, float> size_cache;              // size_y → CoreText の大きさ
	mutable std::map<std::tuple<uint16_t, float>, Glyph> glyph_cache;

	HDFont(const lgfx::IFont *o, const char *n, const char *r) : orig(o), ct_name(n), ref(r) {}

	font_type_t getType(void) const override { return orig->getType(); }
	void getDefaultMetric(lgfx::FontMetrics *m) const override { orig->getDefaultMetric(m); }
	bool updateFontMetric(lgfx::FontMetrics *m, uint16_t c) const override {
		return orig->updateFontMetric(m, c);
	}

	const std::tuple<Ink, size_t> &ink_of(uint16_t c, const lgfx::TextStyle *st,
	                                      const lgfx::FontMetrics *m) const {
		auto key = std::make_tuple(c, st->size_x, st->size_y);
		auto it = ink_cache.find(key);
		if (it == ink_cache.end()) {
			size_t adv = 0;
			Ink k = orig_ink(orig, c, st, m, &adv);
			it = ink_cache.emplace(key, std::make_tuple(k, adv)).first;
		}
		return it->second;
	}

	//	基準の字の高さが元のフォントと同じになる CoreText の大きさ
	float ct_size(const lgfx::TextStyle *st) const {
		auto it = size_cache.find(st->size_y);
		if (it != size_cache.end()) return it->second;
		lgfx::FontMetrics m;
		orig->getDefaultMetric(&m);
		orig->updateFontMetric(&m, (uint8_t)ref[0]);
		const Ink &ok = std::get<0>(ink_of((uint8_t)ref[0], st, &m));
		Glyph g = ct_glyph(ct_name, 100.0f, (uint8_t)ref[0]);
		float sz = 16.0f;
		if (ok.any && g.ink.any) sz = 100.0f * (float)(ok.y1 - ok.y0) / (float)(g.ink.y1 - g.ink.y0);
		size_cache[st->size_y] = sz;
		return sz;
	}

	const Glyph &glyph(uint16_t c, float sz) const {
		auto key = std::make_tuple(c, sz);
		auto it = glyph_cache.find(key);
		if (it == glyph_cache.end()) it = glyph_cache.emplace(key, ct_glyph(ct_name, sz, c)).first;
		return it->second;
	}

	size_t drawChar(lgfx::LGFXBase *gfx, int32_t x, int32_t y, uint16_t c, const lgfx::TextStyle *st,
	                lgfx::FontMetrics *m, int32_t &filled_x) const override {
		std::lock_guard<std::recursive_mutex> g(lock);
		const auto &io = ink_of(c, st, m);
		const Ink &ok = std::get<0>(io);
		const size_t adv = std::get<1>(io);
		if (!ok.any) {
			// 空白など。背景の塗りは元のフォントに任せる
			return orig->drawChar(gfx, x, y, c, st, m, filled_x);
		}
		const Glyph &hg = glyph(c, ct_size(st));
		if (!hg.ink.any) {
			return orig->drawChar(gfx, x, y, c, st, m, filled_x);    // Mac のフォントに無い字
		}
		if (st->back_rgb888 != st->fore_rgb888) {
			// 背景を元と同じ範囲に塗らせ、ビットマップの字形は背景色で消す
			lgfx::FontMetrics mm = *m;
			orig->drawChar(gfx, x, y, c, st, &mm, filled_x);
			lgfx::TextStyle t = *st;
			t.fore_rgb888 = t.back_rgb888;
			int32_t fx = 0;
			mm = *m;
			orig->drawChar(gfx, x, y, c, &t, &mm, fx);
		}

		// インクの中心を合わせる (K 倍の座標なので 2 で割ったあとも整数に近い)
		const float ocx = x + (ok.x0 + ok.x1) * 0.5f, ocy = y + (ok.y0 + ok.y1) * 0.5f;
		const float hcx = (hg.ink.x0 + hg.ink.x1) * 0.5f, hcy = (hg.ink.y0 + hg.ink.y1) * 0.5f;
		const int dx = (int)lroundf(ocx - hcx), dy = (int)lroundf(ocy - hcy);

		int bx0 = dx + hg.ink.x0 - 1, by0 = dy + hg.ink.y0 - 1;
		int bx1 = dx + hg.ink.x1 + 1, by1 = dy + hg.ink.y1 + 1;
		if (bx0 < 0) bx0 = 0;
		if (by0 < 0) by0 = 0;
		if (bx1 > gfx->width()) bx1 = gfx->width();
		if (by1 > gfx->height()) by1 = gfx->height();
		const int bw = bx1 - bx0, bh = by1 - by0;
		if (bw > 0 && bh > 0) {
			std::vector<lgfx::rgb888_t> px((size_t)bw * bh);
			gfx->readRect(bx0, by0, bw, bh, px.data());
			const uint32_t fc = st->fore_rgb888;
			const int fr = (fc >> 16) & 0xFF, fgc = (fc >> 8) & 0xFF, fb = fc & 0xFF;
			for (int yy = 0; yy < bh; yy++) {
				const int gy = by0 + yy - dy;
				if (gy < 0 || gy >= hg.h) continue;
				for (int xx = 0; xx < bw; xx++) {
					const int gx = bx0 + xx - dx;
					if (gx < 0 || gx >= hg.w) continue;
					const int al = hg.a[(size_t)gy * hg.w + gx];
					if (!al) continue;
					auto &p = px[(size_t)yy * bw + xx];
					p.r = (uint8_t)((p.r * (255 - al) + fr * al) / 255);
					p.g = (uint8_t)((p.g * (255 - al) + fgc * al) / 255);
					p.b = (uint8_t)((p.b * (255 - al) + fb * al) / 255);
				}
			}
			gfx->pushImage(bx0, by0, bw, bh, px.data());
		}
		return adv;
	}
};

//	元のフォント → 描き直すフォント。本体のビットマップフォントの元になった書体を
//	そのまま使う (fetch_fonts.sh)。時計の時・分 (Font8) の元は Arial なので Mac の Arial。
//	ボタンの英字 (AsciiFont8x16)、FFT の小さい字 (Font0)、起動画面の小さい字 (Font2) は
//	元から点で作られた書体で元の書体が無いので、表に入れず点のまま 2 倍にする
struct Map {
	const lgfx::IFont *orig;
	const char *ct_name;
	const char *ref;
};

const Map maps[] = {
	{ &fonts::lgfxJapanGothicP_16, "file:ipagp.ttf", "H" },
	{ &fonts::lgfxJapanGothic_24, "file:ipag.ttf", "H" },
	{ &fonts::DejaVu12, "file:DejaVuSans.ttf", "H" },
	{ &fonts::DejaVu18, "file:DejaVuSans.ttf", "H" },
	{ &fonts::DejaVu24, "file:DejaVuSans.ttf", "H" },
	{ &fonts::Orbitron_Light_24, "file:Orbitron-Light.otf", "H" },
	{ &fonts::Orbitron_Light_32, "file:Orbitron-Light.otf", "H" },
	{ &fonts::Font8, "ArialMT", "8" },
	{ &fonts::FreeSansBold24pt7b, "file:FreeSansBold.ttf", "8" },
};

}  // namespace

const lgfx::IFont *hd_font(const lgfx::IFont *orig)
{
	static std::map<const lgfx::IFont *, HDFont *> made;
	std::lock_guard<std::recursive_mutex> g(lock);
	auto it = made.find(orig);
	if (it != made.end()) return it->second;
	for (const auto &mp : maps) {
		if (mp.orig == orig) {
			HDFont *f = new HDFont(orig, mp.ct_name, mp.ref);
			made[orig] = f;
			return f;
		}
	}
	return orig;                            // 対応表に無いフォントは元のまま (段々が出る)
}

}  // namespace simhd
