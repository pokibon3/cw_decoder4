//
//	CW Decoder 4 の PC シミュレータ (Mac / Linux、SDL2)
//
//	src/ のファームウェア (main.cpp の setup()/loop()、画面、DSP、デコーダ) を
//	そのままコンパイルし、ESP32 にしかない部分だけをここで置き換える:
//	  - LCD とタッチ    : SDL のウィンドウとマウス (sim_lgfx.h)
//	  - 音声入力 (ADC)  : 音声ファイル (ffmpeg で 8kHz に変換) か、合成した CW
//	  - NVS             : sim/nvs.txt
//	  - WiFi / NTP / OTA: 電波は使わない。WiFi設定 / FW更新は画面だけ動く。
//	                      時刻は PC の時計 (JST) から取る
//	実機と同じく、ファームウェアの setup()/loop() と DSP タスクは別スレッドで動く
//	(SDL のウィンドウ処理は macOS の都合でメインスレッドに置く)。
//
//	操作: マウスの左ボタン = タッチ / B キー = BOOT ボタン /
//	      S キー = スクリーンショット (sim/shots/ に PNG)
//
#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <stdarg.h>
#include <time.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "audio.h"
#include "clock.h"
#include "decode.h"
#include "display.h"
#include "fwupdate.h"
#include "netsync.h"
#include "ota.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>

void setup(void);
void loop(void);

//==================================================================
//	オプション
//==================================================================
static const char *opt_audio = nullptr;     // 音声ファイル (無ければ合成)
static const char *opt_text = "CQ CQ DE JA1QRP JA1QRP K";
static int opt_wpm = 20;
static int opt_tone = 700;
static float opt_snr = 20.0f;               // 合成時の S/N (dB、BPF 前の帯域 4kHz で)
static float opt_gain = 1.0f;
static std::string opt_script;
static std::string opt_nvs = "nvs.txt";
static std::string opt_shots = "shots";
static int opt_shot_scale = 1;              // 撮るときの拡大率 (画素をそのまま N 倍)

#define SIM_RATE 8000
#define PIN_SHOT 100                        // S キー (LovyanGFX の擬似 GPIO)

static const auto t_start = std::chrono::steady_clock::now();

unsigned long millis(void)
{
	return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now() - t_start).count();
}

//==================================================================
//	Arduino / FreeRTOS スタブの実体
//==================================================================
SerialStub Serial;
EspStub ESP;

int SerialStub::printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	return n;
}

void EspStub::restart(void)
{
	puts("[sim] restart requested - exiting");
	fflush(stdout);
	std::_Exit(0);
}

void pinMode(uint8_t pin, uint8_t mode)
{
	(void)pin;
	(void)mode;
}

int digitalRead(uint8_t pin)
{
	return lgfx::gpio_in(pin) ? HIGH : LOW;
}

//	portMUX はすべて 1 本のロックで代用する (区間は短く、競合も少ない)
static std::recursive_mutex mux_lock;
void sim_mux_enter(portMUX_TYPE *mux) { (void)mux; mux_lock.lock(); }
void sim_mux_exit(portMUX_TYPE *mux) { (void)mux; mux_lock.unlock(); }

void vTaskDelay(TickType_t ticks)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                   uint32_t stack, void *arg, unsigned prio,
                                   void *handle, int core)
{
	(void)name; (void)stack; (void)prio; (void)handle; (void)core;
	std::thread(fn, arg).detach();
	return pdTRUE;
}

struct sim_queue {
	std::mutex m;
	std::deque<std::vector<uint8_t>> q;
	unsigned len, size;
};

QueueHandle_t xQueueCreate(unsigned len, unsigned item_size)
{
	auto *q = new sim_queue;
	q->len = len;
	q->size = item_size;
	return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
	(void)wait;
	std::lock_guard<std::mutex> g(q->m);
	if (q->q.size() >= q->len) return pdFALSE;
	const uint8_t *p = (const uint8_t *)item;
	q->q.emplace_back(p, p + q->size);
	return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
	(void)wait;
	std::lock_guard<std::mutex> g(q->m);
	if (q->q.empty()) return pdFALSE;
	memcpy(item, q->q.front().data(), q->size);
	q->q.pop_front();
	return pdTRUE;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
	static const esp_partition_t part = { "app0", 0x10000 };
	return &part;
}

//==================================================================
//	Preferences (NVS) — 1 行 1 項目「名前空間 キー 16進」のファイル
//==================================================================
static std::mutex nvs_lock;
static std::map<std::string, std::string> nvs;      // "ns/key" -> bytes
static bool nvs_loaded = false;

static void nvs_load(void)
{
	if (nvs_loaded) return;
	nvs_loaded = true;
	FILE *f = fopen(opt_nvs.c_str(), "r");
	if (!f) return;
	char ns[64], key[64], hex[2048];
	while (fscanf(f, "%63s %63s %2047s", ns, key, hex) == 3) {
		std::string v;
		for (size_t i = 0; hex[i] && hex[i + 1] && hex[0] != '-'; i += 2) {
			unsigned b;
			sscanf(hex + i, "%2x", &b);
			v.push_back((char)b);
		}
		nvs[std::string(ns) + "/" + key] = v;
	}
	fclose(f);
}

static void nvs_save(void)
{
	FILE *f = fopen(opt_nvs.c_str(), "w");
	if (!f) return;
	for (auto &kv : nvs) {
		size_t sl = kv.first.find('/');
		fprintf(f, "%s %s ", kv.first.substr(0, sl).c_str(), kv.first.substr(sl + 1).c_str());
		if (kv.second.empty()) fputc('-', f);
		for (unsigned char c : kv.second) fprintf(f, "%02x", c);
		fputc('\n', f);
	}
	fclose(f);
}

bool Preferences::begin(const char *ns, bool read_only)
{
	(void)read_only;
	ns_ = ns;
	std::lock_guard<std::mutex> g(nvs_lock);
	nvs_load();
	return true;
}

bool Preferences::get(const char *key, std::string *out)
{
	std::lock_guard<std::mutex> g(nvs_lock);
	auto it = nvs.find(ns_ + "/" + key);
	if (it == nvs.end()) return false;
	*out = it->second;
	return true;
}

void Preferences::put(const char *key, const std::string &v)
{
	std::lock_guard<std::mutex> g(nvs_lock);
	nvs[ns_ + "/" + key] = v;
	nvs_save();
}

bool Preferences::isKey(const char *key)
{
	std::string v;
	return get(key, &v);
}

bool Preferences::remove(const char *key)
{
	std::lock_guard<std::mutex> g(nvs_lock);
	bool had = nvs.erase(ns_ + "/" + key) > 0;
	nvs_save();
	return had;
}

bool Preferences::clear(void)
{
	std::lock_guard<std::mutex> g(nvs_lock);
	std::string pre = ns_ + "/";
	for (auto it = nvs.begin(); it != nvs.end();) {
		if (it->first.compare(0, pre.size(), pre) == 0) it = nvs.erase(it);
		else ++it;
	}
	nvs_save();
	return true;
}

uint8_t Preferences::getUChar(const char *key, uint8_t def)
{
	std::string v;
	return (get(key, &v) && v.size() == 1) ? (uint8_t)v[0] : def;
}

size_t Preferences::putUChar(const char *key, uint8_t v)
{
	put(key, std::string(1, (char)v));
	return 1;
}

String Preferences::getString(const char *key, const String &def)
{
	std::string v;
	return get(key, &v) ? String(v) : def;
}

size_t Preferences::putString(const char *key, const char *v)
{
	put(key, std::string(v));
	return strlen(v);
}

size_t Preferences::getBytes(const char *key, void *buf, size_t len)
{
	std::string v;
	if (!get(key, &v) || v.size() > len) return 0;
	memcpy(buf, v.data(), v.size());
	return v.size();
}

size_t Preferences::putBytes(const char *key, const void *buf, size_t len)
{
	put(key, std::string((const char *)buf, len));
	return len;
}

//==================================================================
//	WiFi / NTP / 自動更新 / OTA の代わり
//==================================================================
static uint32_t synced_epoch = 0;

void netsync_init(void) { }
void netsync_poll(void) { }
bool netsync_sync_now(bool redraw_clock) { (void)redraw_clock; return false; }
bool netsync_has_wifi(void) { return false; }
void netsync_save_wifi(const char *ssid, const char *pass) { (void)ssid; (void)pass; }
void netsync_clear_wifi(void) { }
String netsync_ssid(void) { return String(""); }
String netsync_pass(void) { return String(""); }
uint32_t netsync_last_epoch(void) { return synced_epoch; }
void netsync_note_synced(uint32_t epoch) { synced_epoch = epoch; }

static bool upd_check = true;
void fwupdate_init(void) { }
bool fwupdate_check_enabled(void) { return upd_check; }
void fwupdate_set_check_enabled(bool on) { upd_check = on; }

//	実機は起動時にここで NTP 同期する。PC の時計を JST に直して入れる
void fwupdate_check_on_boot(LGFX *lcd)
{
	(void)lcd;
	uint32_t jst = (uint32_t)time(nullptr) + NETSYNC_TZ_OFFSET;
	clock_set(jst);
	netsync_note_synced(jst);
	delay(1500);                            // 実機の起動チェックの間、起動画面が出ている
}

//	WiFi設定 / FW更新 (ota.cpp) は画面だけ動く。電波は出さず、端末もつながってこない
//	(shim/WiFi.h ほか)。[キャンセル] で SETUP へ戻る
WiFiClass WiFi;
MDNSResponder MDNS;
UpdateClass Update;

//==================================================================
//	音声入力 (audio.h の実装)
//	供給スレッドが実時間で 8kHz のサンプルを積み、DSP タスクが
//	audio_read() で取り出す。実機のアナログ BPF (700Hz, Q=5) もここで掛ける
//==================================================================
#define RING_SIZE 16384
static std::mutex ring_lock;
static std::condition_variable ring_cv;
static std::deque<uint16_t> ring;
static std::atomic<uint32_t> clip_total{0};

void audio_init(void) { }
void audio_stop(void) { }
uint32_t audio_clip_total(void) { return clip_total; }

size_t audio_read(uint16_t *dst, size_t n)
{
	std::unique_lock<std::mutex> g(ring_lock);
	ring_cv.wait(g, [&] { return ring.size() >= n; });
	for (size_t i = 0; i < n; i++) {
		dst[i] = ring.front();
		ring.pop_front();
	}
	return n;
}

struct Biquad {
	float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
	// RBJ bandpass (定ピークゲイン 0dB)
	void bandpass(float f0, float q, float fs) {
		float w = 2.0f * (float)M_PI * f0 / fs, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
		b0 = al / a0; b1 = 0; b2 = -al / a0;
		a1 = -2.0f * cosf(w) / a0; a2 = (1.0f - al) / a0;
	}
	float run(float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

//	全角カタカナ 1 文字 (UTF-8) を JIS X 0201 のカナ (1〜2 バイト) にする。
//	濁音・半濁音は清音 + ゛/゜、小書きは並字にする (和文電信と同じ)
static int kana_to_jis(const char *p, uint8_t *out, int *nout)
{
	static const char *base =
		"ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノ"
		"ハヒフヘホマミムメモヤユヨラリルレロワン";              // 0xA6〜0xDD
	static const char *daku = "ガギグゲゴザジズゼゾダヂヅデドバビブベボ";
	static const char *dbase = "カキクケコサシスセソタチツテトハヒフヘホ";
	static const char *han = "パピプペポ";
	static const char *hbase = "ハヒフヘホ";
	static const char *small = "ァィゥェォャュョッ";
	static const char *large = "アイウエオヤユヨツ";
	auto idx = [](const char *tbl, const char *c) -> int {
		for (int i = 0; tbl[i * 3]; i++) {
			if (!memcmp(tbl + i * 3, c, 3)) return i;
		}
		return -1;
	};
	auto jis = [&](const char *c) -> uint8_t {
		int i = idx(base, c);
		return (i < 0) ? 0 : (uint8_t)(0xA6 + i);
	};
	if ((uint8_t)p[0] < 0xE0) return 0;          // 3 バイトの文字だけを扱う
	*nout = 0;
	int i;
	if ((i = idx(daku, p)) >= 0) {
		out[0] = jis(dbase + i * 3);
		out[1] = 0xDE;
		*nout = 2;
	} else if ((i = idx(han, p)) >= 0) {
		out[0] = jis(hbase + i * 3);
		out[1] = 0xDF;
		*nout = 2;
	} else if (!memcmp(p, "ヴ", 3)) {
		out[0] = jis("ウ");
		out[1] = 0xDE;
		*nout = 2;
	} else if ((i = idx(small, p)) >= 0) {
		out[0] = jis(large + i * 3);
		*nout = 1;
	} else if (!memcmp(p, "、", 3)) {
		out[0] = 0xA4;
		*nout = 1;
	} else if (!memcmp(p, "」", 3)) {
		out[0] = 0xA3;
		*nout = 1;
	} else if ((out[0] = jis(p)) != 0) {
		*nout = 1;
	}
	return 3;
}

//	合成 CW: 文字列をモールスにして延々と繰り返す (-1..1)。
//	符号はファームウェアの表 (decode.cpp の encode_*) から引くので、
//	デコーダが読めるものはそのまま送れる。カナが出てくると「ホレ」、
//	和文の途中で英字が出てくると「ラタ」を自動で挟む
static void synth_cw(std::vector<float> *out)
{
	const int unit = SIM_RATE * 6 / (5 * opt_wpm);  // 1.2 / WPM 秒
	const int ramp = SIM_RATE * 5 / 1000;
	std::vector<uint8_t> key;
	auto on = [&](int u) { key.insert(key.end(), (size_t)(u * unit), 1); };
	auto off = [&](int u) { key.insert(key.end(), (size_t)(u * unit), 0); };
	auto send = [&](const char *code) {
		if (!code) return;
		for (const char *e = code; *e; e++) {
			on(*e == '-' ? 3 : 1);
			off(1);
		}
		off(2);                                 // 文字間 = 1 + 2
	};
	bool jp = false;
	off(7);
	for (const char *p = opt_text; *p;) {
		uint8_t k[2];
		int nk = 0;
		int len = kana_to_jis(p, k, &nk);
		if (len > 0) {
			p += len;
			if (nk == 0) continue;              // 和文の表に無い文字
			if (!jp) { send(encode_us_data(5)); jp = true; }    // ホレ
			for (int i = 0; i < nk; i++) send(encode_jp_char(k[i]));
			continue;
		}
		char c = *p++;
		if (c == ' ') { off(4); continue; }     // 語間 = 文字間 3 + 4
		if ((uint8_t)c >= 0x80) continue;
		if (jp && isalpha((unsigned char)c)) { send(encode_jp_data(6)); jp = false; }  // ラタ
		send(jp ? encode_jp_char((uint8_t)c) : encode_us_char((char)toupper((unsigned char)c)));
	}
	if (jp) send(encode_jp_data(6));            // 次の繰り返しは欧文から始まる
	off(7);
	out->resize(key.size());
	float env = 0;
	for (size_t i = 0; i < key.size(); i++) {
		env += (key[i] ? 1.0f : -1.0f) / (float)ramp;
		env = env < 0 ? 0 : (env > 1 ? 1 : env);
		(*out)[i] = 0.5f * env * sinf(2.0f * (float)M_PI * (float)opt_tone * (float)i / SIM_RATE);
	}
}

static float gauss(void)
{
	float u1 = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
	float u2 = (float)rand() / (float)RAND_MAX;
	return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

static FILE *open_audio(void)
{
	std::string q = "'";
	for (const char *p = opt_audio; *p; p++) {
		if (*p == '\'') q += "'\\''";
		else q += *p;
	}
	q += "'";
	// 終了時の Broken pipe を出さないよう ffmpeg のメッセージは捨てる。
	// 読めなかったことは最初の読み出しで分かる (audio_feeder)
	std::string cmd = "ffmpeg -nostdin -v quiet -i " + q + " -map 0:a -ac 1 -ar 8000 -f s16le - 2>/dev/null";
	FILE *f = popen(cmd.c_str(), "r");
	if (!f) {
		fprintf(stderr, "[sim] ffmpeg を起動できません\n");
		std::_Exit(1);
	}
	return f;
}

static void push_sample(float v)
{
	int c = 2048 + (int)lrintf(v * 2048.0f * opt_gain);
	if (c <= 4 || c >= 4090) clip_total++;
	c = c < 0 ? 0 : (c > 4095 ? 4095 : c);
	std::lock_guard<std::mutex> g(ring_lock);
	ring.push_back((uint16_t)c);
	if (ring.size() > RING_SIZE) ring.pop_front();     // DSP 停止中は古い方を捨てる
}

static void audio_feeder(void)
{
	Biquad bpf;
	bpf.bandpass(700.0f, 5.0f, SIM_RATE);
	std::vector<float> cw;
	if (!opt_audio) synth_cw(&cw);
	const float noise = 0.5f / sqrtf(2.0f) * powf(10.0f, -opt_snr / 20.0f);

	FILE *f = opt_audio ? open_audio() : nullptr;
	size_t pos = 0;
	bool got_any = false;
	const int chunk = SIM_RATE / 100;       // 10ms ごとに積む
	auto next = std::chrono::steady_clock::now();
	for (;;) {
		for (int i = 0; i < chunk; i++) {
			float x;
			if (f) {
				int16_t s;
				if (fread(&s, 2, 1, f) != 1) {  // 終わったら頭から繰り返す
					pclose(f);
					if (!got_any) {
						fprintf(stderr, "[sim] %s を読めません (ffmpeg が要ります)\n", opt_audio);
						std::_Exit(1);
					}
					f = open_audio();
					s = 0;
				}
				got_any = true;
				x = (float)s / 32768.0f;
			} else {
				x = cw[pos] + noise * gauss();
				pos = (pos + 1) % cw.size();
			}
			push_sample(bpf.run(x));
		}
		ring_cv.notify_all();
		next += std::chrono::milliseconds(10);
		std::this_thread::sleep_until(next);
	}
}

//==================================================================
//	スクリーンショットと台本
//==================================================================
static std::mutex shot_lock;
static std::deque<std::string> shot_req;    // UI スレッドが delay() の中で撮る
static std::thread::id ui_thread;

static std::string shot_name(void)
{
	time_t t = time(nullptr);
	char buf[64];
	strftime(buf, sizeof(buf), "shot-%Y%m%d-%H%M%S.png", localtime(&t));
	return opt_shots + "/" + buf;
}

static void take_shot(const std::string &path)
{
	LGFX *lcd = display_lcd();
	size_t len = 0;
	void *png;
#if defined(CWDEC_SIM_HD)
	png = lcd->sim_png(&len);               // 2 倍で描いてあるのでそのまま
#else
	if (opt_shot_scale <= 1) {
		png = lcd->createPng(&len, 0, 0, lcd->width(), lcd->height());
	} else {
		// 補間せず画素を N×N に広げる (文字の輪郭をぼかさない)。
		// README では 2 倍の画像をちょうど半分の幅で表示するので、実機と同じ画素になる
		const int w0 = lcd->width(), h0 = lcd->height(), k = opt_shot_scale;
		const int w = w0 * k, h = h0 * k;
		std::vector<lgfx::rgb888_t> line(w0);
		lgfx::LGFX_Sprite big;
		big.setColorDepth(24);
		if (!big.createSprite(w, h)) {
			fprintf(stderr, "[sim] 拡大用のメモリが取れません\n");
			return;
		}
		for (int y = 0; y < h0; y++) {
			lcd->readRect(0, y, w0, 1, line.data());
			for (int x = 0; x < w0; x++) {
				big.fillRect(x * k, y * k, k, k, big.color888(line[x].r, line[x].g, line[x].b));
			}
		}
		png = big.createPng(&len, 0, 0, w, h);
	}
#endif
	if (!png) {
		fprintf(stderr, "[sim] PNG を作れませんでした\n");
		return;
	}
	std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
	if (!dir.empty()) system(("mkdir -p '" + dir + "'").c_str());
	FILE *f = fopen(path.c_str(), "wb");
	if (f) {
		fwrite(png, 1, len, f);
		fclose(f);
		printf("[sim] saved %s\n", path.c_str());
		fflush(stdout);
	} else {
		fprintf(stderr, "[sim] %s に書けません\n", path.c_str());
	}
	free(png);
}

//	ファームウェアの待ち (loop の 1 フレームや画面ごとの待ち) はすべてここを通る。
//	描画と同じスレッドなので、撮影はここで行う (描画途中の画面を読まない)
void delay(unsigned long ms)
{
	if (std::this_thread::get_id() == ui_thread) {
		static bool shot_key = false;
		bool k = !lgfx::gpio_in(PIN_SHOT);
		if (k && !shot_key) {
			std::lock_guard<std::mutex> g(shot_lock);
			shot_req.push_back(shot_name());
		}
		shot_key = k;
		std::deque<std::string> req;
		{
			std::lock_guard<std::mutex> g(shot_lock);
			req.swap(shot_req);
		}
		for (auto &p : req) take_shot(p);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

//	論理座標 (320x240 横) をパネル素の向き (240x320) の生値へ戻す。
//	setRotation(1) の逆変換 (Panel_Device::convertRawXY 参照)
static void logical_to_raw(int lx, int ly, int16_t *rx, int16_t *ry)
{
	*rx = (int16_t)(240 * SIM_K - 1 - ly * SIM_K);
	*ry = (int16_t)(lx * SIM_K);
}

static void tap(int x, int y, int ms, int gap_ms = 100)
{
	Panel_sim *p = display_lcd()->sim_panel();
	logical_to_raw(x, y, (int16_t *)&p->inject_x, (int16_t *)&p->inject_y);
	p->inject = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	p->inject = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
}

//	台本: 「;」か改行で区切った命令を順に実行する (# で始まる行は注釈)
//	  wait 秒 / tap x y / dtap x y (ダブルタップ) / hold x y 秒 / boot /
//	  shot ファイル名 / quit
static void run_script(void)
{
	std::string s = opt_script;
	for (char &c : s) if (c == ';') c = '\n';
	size_t pos = 0;
	while (pos < s.size()) {
		size_t e = s.find('\n', pos);
		if (e == std::string::npos) e = s.size();
		std::string line = s.substr(pos, e - pos);
		pos = e + 1;
		char cmd[32] = "", arg[512] = "";
		float a = 0, b = 0, c = 0;
		if (sscanf(line.c_str(), "%31s", cmd) != 1 || cmd[0] == '#') continue;
		if (!strcmp(cmd, "wait") && sscanf(line.c_str(), "%*s %f", &a) == 1) {
			std::this_thread::sleep_for(std::chrono::milliseconds((int)(a * 1000)));
		} else if (!strcmp(cmd, "tap") && sscanf(line.c_str(), "%*s %f %f", &a, &b) == 2) {
			tap((int)a, (int)b, 150);
		} else if (!strcmp(cmd, "dtap") && sscanf(line.c_str(), "%*s %f %f", &a, &b) == 2) {
			tap((int)a, (int)b, 80, 120);   // 2 打で DTAP_MS (400ms) に収める
			tap((int)a, (int)b, 80, 120);
		} else if (!strcmp(cmd, "hold") && sscanf(line.c_str(), "%*s %f %f %f", &a, &b, &c) == 3) {
			tap((int)a, (int)b, (int)(c * 1000));
		} else if (!strcmp(cmd, "boot")) {
			lgfx::gpio_lo(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			lgfx::gpio_hi(0);
		} else if (!strcmp(cmd, "shot") && sscanf(line.c_str(), "%*s %511s", arg) == 1) {
			{
				std::lock_guard<std::mutex> g(shot_lock);
				shot_req.push_back(strchr(arg, '/') ? std::string(arg) : opt_shots + "/" + arg);
			}
			// 撮り終わるまで待つ (UI スレッドが次の delay() で撮る)
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				std::lock_guard<std::mutex> g(shot_lock);
				if (shot_req.empty()) break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		} else if (!strcmp(cmd, "quit")) {
			fflush(stdout);
			std::_Exit(0);
		} else {
			fprintf(stderr, "[sim] 台本の行が読めません: %s\n", line.c_str());
		}
	}
}

//==================================================================
//	main
//==================================================================
static void usage(void)
{
	fputs(
	"使い方: cwsim [オプション] [音声ファイル]\n"
	"  音声ファイル     mp3 / wav / m4a など (ffmpeg で読む)。省略時は合成した CW\n"
	"  --text 文字列    合成する文 (既定: CQ CQ DE JA1QRP JA1QRP K)。カタカナも可\n"
	"  --wpm N          合成の速さ (既定 20)\n"
	"  --tone Hz        合成のトーン (既定 700)\n"
	"  --snr dB         合成の S/N (既定 20)\n"
	"  --gain 倍率      入力レベル (既定 1.0。1.0 で音声のフルスケールが ADC のフルスケール)\n"
	"  --nvs ファイル   設定の保存先 (既定 nvs.txt)\n"
	"  --shots フォルダ スクリーンショットの保存先 (既定 shots)\n"
	"  --shot-scale N   スクリーンショットを N 倍で保存 (既定 1 = 320x240)\n"
	"  --script 台本    「;」区切りの命令: wait 秒 / tap x y / dtap x y / hold x y 秒 /\n"
	"                   boot / shot ファイル名 / quit。@ファイル名 でファイルから読む\n"
	"操作: マウス = タッチ / B キー = BOOT ボタン / S キー = スクリーンショット\n",
	stderr);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char * {
			if (i + 1 >= argc) { usage(); exit(2); }
			return argv[++i];
		};
		if (a == "--text") opt_text = next();
		else if (a == "--wpm") opt_wpm = atoi(next());
		else if (a == "--tone") opt_tone = atoi(next());
		else if (a == "--snr") opt_snr = (float)atof(next());
		else if (a == "--gain") opt_gain = (float)atof(next());
		else if (a == "--nvs") opt_nvs = next();
		else if (a == "--shots") opt_shots = next();
		else if (a == "--shot-scale") opt_shot_scale = atoi(next());
		else if (a == "--script") opt_script = next();
		else if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (a[0] == '-') { usage(); return 2; }
		else opt_audio = argv[i];
	}
	if (!opt_script.empty() && opt_script[0] == '@') {
		FILE *f = fopen(opt_script.c_str() + 1, "r");
		if (!f) { fprintf(stderr, "[sim] 台本 %s を開けません\n", opt_script.c_str() + 1); return 2; }
		std::string s;
		char buf[512];
		while (fgets(buf, sizeof(buf), f)) s += buf;
		fclose(f);
		opt_script = s;
	}
	if (opt_wpm < 5) opt_wpm = 5;

	lgfx::Panel_sdl::setup();
	lgfx::Panel_sdl::addKeyCodeMapping(SDLK_b, 0);
	lgfx::Panel_sdl::addKeyCodeMapping(SDLK_s, PIN_SHOT);

	std::thread(audio_feeder).detach();
	std::thread fw([] {
		ui_thread = std::this_thread::get_id();
		// 台本は setup() の前から動かす (起動画面も撮れるように)
		if (!opt_script.empty()) std::thread(run_script).detach();
		setup();
		for (;;) loop();
	});
	fw.detach();

	while (lgfx::Panel_sdl::loop() == 0) { }
	fflush(stdout);
	std::_Exit(0);
}
