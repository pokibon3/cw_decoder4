//
//	起動時のオンラインアップデートチェック本体
//
//	画面: スプラッシュをそのまま出したまま、空けてある最下段に書き足す。
//	  y 205..219  進捗バー (ダウンロード中だけ。Build 行の位置を使う)
//	  y 223..239  ステータス行 (WiFi接続 / 時刻同期 / 更新の確認 / 受信中 %)
//
//	配布物 (GitHub Pages, tools/make_release.sh が作る):
//	  firmware/latest.txt          version / build / パネル別の size, md5
//	  firmware/firmware_ST7789.bin
//	  firmware/firmware_ILI9341.bin
//
//	更新の安全性: ota.cpp と同じく Update は非アクティブ側のアプリ
//	パーティションへ書き、end() でサイズと MD5 を検証してから起動
//	パーティションを切り替える。途中で切れても旧ファームが残る。
//	さらに latest.txt に載せた MD5 を Update.setMD5() に渡すので、
//	配布物と1バイトでも違えば書き換えは成立しない。
//
//	TLS: サーバ証明書の検証はしていない (setInsecure)。改竄検出は
//	上記 MD5 に頼る形。証明書を焼き込んで固定すると、CA が変わった
//	ときに「更新できないファームを更新で直せない」状態になるため、
//	自動更新の経路としてはこの形を選んだ。検証を入れるなら
//	http_begin() の setInsecure() を setCACert(<root CA PEM>) に
//	差し替えるだけでよい。
//
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>
#include "fwupdate.h"
#include "netsync.h"
#include "clock.h"
#include "display.h"
#include "version.h"

#define NVS_NS "cwdec"
#define NVS_KEY "upd_chk"

#define FW_URL_BASE "https://pokibon3.github.io/cw_decoder4/firmware/"
#define MANIFEST_URL FW_URL_BASE "latest.txt"

#define WIFI_TIMEOUT_MS 15000UL
#define SNTP_TIMEOUT_MS 8000UL
#define HTTP_TIMEOUT_MS 10000UL

#define C_ACCENT lgfx::color565(255, 208, 120)
#define C_OK     lgfx::color565(120, 220, 160)
#define C_ERR    lgfx::color565(240, 110, 100)
#define C_DIM    lgfx::color565(120, 140, 165)
#define C_FRAME  lgfx::color565(51, 69, 92)

// スプラッシュの最下段に状況を出す。ダウンロード中だけ、その上の
// Build 行の位置を進捗バーに使う (受信中は版数の表示より進み具合が要る)
#define STAT_Y 223
#define BAR_X 60
#define BAR_Y 205
#define BAR_W 200
#define BAR_H 14

static LGFX *lcd;
static bool check_enabled = true;

//	----- 設定 (NVS) -----

void fwupdate_init(void)
{
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		if (prefs.isKey(NVS_KEY)) {
			check_enabled = (prefs.getUChar(NVS_KEY, 1) != 0);
		}
		prefs.end();
	}
	Serial.printf("[upd] boot check %s\n", check_enabled ? "ON" : "OFF");
}

bool fwupdate_check_enabled(void)
{
	return check_enabled;
}

void fwupdate_set_check_enabled(bool on)
{
	check_enabled = on;
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		prefs.putUChar(NVS_KEY, on ? 1 : 0);
		prefs.end();
	}
}

//	----- 画面 -----

static void draw_base(void)
{
	display_splash_draw();          // スプラッシュはそのまま出したままにする
}

static void draw_status(const char *msg, uint16_t color)
{
	lcd->fillRect(0, STAT_Y, 320, 240 - STAT_Y, DISPLAY_SPLASH_BG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(color, DISPLAY_SPLASH_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	lcd->drawString(msg, 160, STAT_Y);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
}

//	Build 行の位置に進捗バーを描く (最初の一度だけその行を消す)
static void draw_bar(int pct)
{
	if (pct == 0) {
		lcd->fillRect(0, BAR_Y - 2, 320, 18, DISPLAY_SPLASH_BG);
	}
	lcd->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_FRAME);
	int w = (BAR_W - 4) * pct / 100;
	lcd->fillRect(BAR_X + 2, BAR_Y + 2, w, BAR_H - 4, C_ACCENT);
	lcd->fillRect(BAR_X + 2 + w, BAR_Y + 2, BAR_W - 4 - w, BAR_H - 4, DISPLAY_SPLASH_BG);
}

//	----- HTTP -----

//	GitHub Pages への GET を用意する (証明書は検証しない: 冒頭の注記を参照)
static bool http_begin(HTTPClient &http, WiFiClientSecure &client, const String &url)
{
	client.setInsecure();
	client.setTimeout(HTTP_TIMEOUT_MS / 1000);
	http.setTimeout(HTTP_TIMEOUT_MS);
	http.setConnectTimeout(HTTP_TIMEOUT_MS);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	return http.begin(client, url);
}

//	latest.txt の "key=value" を1つ取り出す (無ければ空文字)
static String manifest_value(const String &body, const String &key)
{
	String pat = "\n" + key + "=";
	String b = "\n" + body;             // 先頭行も同じ形で拾えるようにする
	int i = b.indexOf(pat);
	if (i < 0) {
		return String();
	}
	i += pat.length();
	int e = b.indexOf('\n', i);
	if (e < 0) {
		e = b.length();
	}
	String v = b.substring(i, e);
	v.trim();                           // 行末の CR を落とす
	return v;
}

//	"2.6" と "2.5" を major.minor で比べる
static bool version_is_newer(const String &remote, const char *local)
{
	int rmaj = 0, rmin = 0, lmaj = 0, lmin = 0;
	if (sscanf(remote.c_str(), "%d.%d", &rmaj, &rmin) < 1) {
		return false;
	}
	sscanf(local, "%d.%d", &lmaj, &lmin);
	if (rmaj != lmaj) {
		return rmaj > lmaj;
	}
	return rmin > lmin;
}

//	----- 各ステップ -----

static void sync_time(void)
{
	draw_status("時刻を同期しています ...", C_ACCENT);
	configTime(NETSYNC_TZ_OFFSET, 0, "ntp.nict.jp", "pool.ntp.org");

	struct tm ti;
	if (!getLocalTime(&ti, SNTP_TIMEOUT_MS)) {
		Serial.println("[upd] sntp timeout");
		draw_status("時刻同期に失敗しました", C_ERR);
		delay(800);
		return;
	}

	clock_tm_t t;
	t.year = (int16_t)(ti.tm_year + 1900);
	t.mon = (uint8_t)(ti.tm_mon + 1);
	t.day = (uint8_t)ti.tm_mday;
	t.hour = (uint8_t)ti.tm_hour;
	t.min = (uint8_t)ti.tm_min;
	t.sec = (uint8_t)ti.tm_sec;
	t.wday = 0;
	uint32_t epoch = clock_make(&t);
	clock_set(epoch);
	netsync_note_synced(epoch);     // 直後に日次同期が二重で走らないようにする

	char buf[48];
	snprintf(buf, sizeof(buf), "時刻同期 %02d:%02d:%02d", t.hour, t.min, t.sec);
	Serial.printf("[upd] ntp ok: %04d-%02d-%02d %s\n", t.year, t.mon, t.day, buf);
	draw_status(buf, C_OK);
}

//	bin を取ってきて非アクティブ側へ書く。成功したら true (呼び出し側が再起動する)
static bool download_and_flash(const String &url, size_t size, const String &md5)
{
	WiFiClientSecure client;
	HTTPClient http;

	if (!http_begin(http, client, url)) {
		draw_status("ダウンロードを開始できません", C_ERR);
		return false;
	}
	int code = http.GET();
	if (code != HTTP_CODE_OK) {
		Serial.printf("[upd] bin GET failed: %d\n", code);
		draw_status("ダウンロードに失敗しました", C_ERR);
		http.end();
		return false;
	}

	int len = http.getSize();
	if (len <= 0) {
		len = (int)size;            // chunked 等でサイズが取れないときは latest.txt の値
	}
	Serial.printf("[upd] downloading %d bytes\n", len);

	if (!Update.begin((size_t)len)) {
		Update.printError(Serial);
		draw_status("書き込み領域が足りません", C_ERR);
		http.end();
		return false;
	}
	if (md5.length() == 32) {
		Update.setMD5(md5.c_str());     // 配布物と一致しなければ end() で弾かれる
	}

	draw_bar(0);
	WiFiClient *stream = http.getStreamPtr();
	uint8_t buf[1024];
	size_t written = 0;
	int last_pct = -1;
	uint32_t t_last = millis();

	while (written < (size_t)len) {
		size_t avail = stream->available();
		if (avail) {
			size_t n = avail > sizeof(buf) ? sizeof(buf) : avail;
			int got = stream->readBytes(buf, n);
			if (got <= 0) {
				break;
			}
			if (Update.write(buf, got) != (size_t)got) {
				Update.printError(Serial);
				break;
			}
			written += got;
			t_last = millis();
			int pct = (int)(written * 100 / (size_t)len);
			if (pct != last_pct) {
				last_pct = pct;
				draw_bar(pct);
				char msg[32];
				snprintf(msg, sizeof(msg), "受信中 %d%%", pct);
				draw_status(msg, C_ACCENT);
			}
		} else if (!http.connected() || millis() - t_last > HTTP_TIMEOUT_MS) {
			Serial.println("[upd] download stalled");
			break;
		} else {
			delay(10);
		}
	}
	http.end();

	if (written != (size_t)len) {
		Serial.printf("[upd] truncated: %u / %d\n", (unsigned)written, len);
		Update.abort();
		draw_status("通信が途中で切れました", C_ERR);
		return false;
	}
	if (!Update.end(true)) {
		Update.printError(Serial);
		draw_status("検証に失敗しました", C_ERR);
		return false;
	}
	return true;
}

//	latest.txt を見て、新版があれば確認のうえ更新する
static void check_and_update(void)
{
	draw_status("更新を確認しています ...", C_ACCENT);

	String body;
	{
		WiFiClientSecure client;
		HTTPClient http;
		if (!http_begin(http, client, MANIFEST_URL)) {
			draw_status("更新はありません", C_DIM);
			delay(800);
			return;
		}
		int code = http.GET();
		if (code != HTTP_CODE_OK) {
			Serial.printf("[upd] manifest GET failed: %d\n", code);
			draw_status("更新はありません", C_DIM);
			http.end();
			delay(800);
			return;
		}
		body = http.getString();
		http.end();
	}

	String remote = manifest_value(body, "version");
	if (!remote.length()) {
		Serial.println("[upd] manifest has no version");
		draw_status("更新はありません", C_DIM);
		delay(800);
		return;
	}
	Serial.printf("[upd] local v%s / remote v%s\n", FW_VERSION, remote.c_str());

	if (!version_is_newer(remote, FW_VERSION)) {
		draw_status("最新版です (v" FW_VERSION ")", C_OK);
		delay(800);
		return;
	}

	String size_s = manifest_value(body, String(FW_PANEL) + "_size");
	String md5 = manifest_value(body, String(FW_PANEL) + "_md5");
	size_t size = (size_t)size_s.toInt();
	if (!size) {
		Serial.printf("[upd] manifest has no size for %s\n", FW_PANEL);
		draw_status("このパネル用の配布物がありません", C_ERR);
		delay(1200);
		return;
	}

	char line1[64];
	snprintf(line1, sizeof(line1), "v%s  →  v%s", FW_VERSION, remote.c_str());
	if (!display_confirm(lcd, "新しいファームウェアがあります",
	                     line1, "更新すると再起動します", "更新")) {
		draw_base();
		draw_status("更新しませんでした", C_ACCENT);
		delay(600);
		return;
	}

	draw_base();
	String url = String(FW_URL_BASE) + "firmware_" + FW_PANEL + ".bin";
	if (!download_and_flash(url, size, md5)) {
		delay(1500);                // 失敗の理由を読ませてから通常起動を続ける
		return;
	}

	draw_bar(100);
	draw_status("更新しました 再起動します", C_OK);
	Serial.println("[upd] success, restarting");
	delay(1200);
	ESP.restart();
}

//	----- エントリ -----

void fwupdate_check_on_boot(LGFX *lcd_)
{
	if (!check_enabled || !netsync_has_wifi()) {
		return;
	}
	lcd = lcd_;

	draw_status("WiFi に接続しています ...", C_ACCENT);
	Serial.printf("[upd] connect to %s (heap=%u)\n", netsync_ssid().c_str(),
	              (unsigned)ESP.getFreeHeap());

	WiFi.persistent(false);         // 資格情報を NVS に書かない
	WiFi.mode(WIFI_STA);
	WiFi.begin(netsync_ssid().c_str(), netsync_pass().c_str());

	uint32_t t0 = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
		delay(100);
	}

	if (WiFi.status() == WL_CONNECTED) {
		sync_time();
		check_and_update();         // 更新したときは戻らない (再起動)
	} else {
		Serial.println("[upd] wifi connect failed");
		draw_status("WiFi に接続できませんでした", C_ERR);
		delay(1200);
	}

	// 通常動作では無線 OFF が原則。必ず落としてから戻る
	// (デコーダ画面への切り替えは呼び出し側が行う)
	WiFi.disconnect(true, true);
	WiFi.mode(WIFI_OFF);
	delay(50);
	Serial.printf("[upd] wifi off, heap=%u\n", (unsigned)ESP.getFreeHeap());
}
