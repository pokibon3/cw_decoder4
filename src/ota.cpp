//
//	OTA モード / WiFi 設定モード本体
//
//	画面 (320x240 横):
//	  y   0..26   タイトル行 (OTA モード or WiFi設定 / バージョン)
//	  y  32..150  左: SSID / PASS / URL / 接続台数   右: WiFi接続用QR
//	  y 168..239  ステータス + 進捗バー (OTA) / [キャンセル] ボタン (WiFi設定)
//
//	WiFi 設定モードは Web ページで保存されるか本体の [キャンセル] が
//	押されると AP を落として呼び出し元 (SETUP 画面) へ戻る。
//
//	更新の安全性: Update は非アクティブ側のアプリパーティション
//	(app0/app1 の空いている方) に書き込み、end() でサイズと MD5 を
//	検証して初めて起動パーティションを切り替える。途中で通信断や
//	電源断が起きても旧ファームがそのまま残る。
//
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <DNSServer.h>
#include "ota.h"
#include "netsync.h"
#include "version.h"

#define C_BG      lgfx::color565(14, 17, 22)
#define C_TITLEBG lgfx::color565(26, 34, 46)
#define C_TITLETX lgfx::color565(200, 212, 226)
#define C_LABEL   lgfx::color565(120, 140, 165)
#define C_VALUE   lgfx::color565(235, 240, 246)
#define C_ACCENT  lgfx::color565(255, 208, 120)
#define C_OK      lgfx::color565(120, 220, 160)
#define C_ERR     lgfx::color565(240, 110, 100)
#define C_FRAME   lgfx::color565(51, 69, 92)

#define BAR_X 8
#define BAR_Y 201
#define BAR_W 184
#define BAR_H 22
#define STAT_Y 168
#define INFO_Y 148
// WiFi設定モードの [キャンセル] ボタン (進捗バーの位置に置く)
#define CANCEL_X 200
#define CANCEL_Y 194
#define CANCEL_W 112
#define CANCEL_H 36
#define C_BTN_BG  lgfx::color565(26, 34, 46)
#define C_BTN_BD  lgfx::color565(70, 92, 120)
#define C_BTN_TX  lgfx::color565(190, 206, 226)

static LGFX *lcd;
static WebServer server(80);
// キャプティブポータル: 全ホスト名を AP の IP に解決し、OS の接続確認
// (Android generate_204 / iOS hotspot-detect 等) を自分のページへ
// リダイレクトする。これで AP に繋いだ端末に「サインイン」通知が出て
// 設定ページが自動で開く (QR で接続するだけでブラウザ操作が要らない)
static DNSServer dns;
static size_t up_total;         // Content-Length (multipart のヘッダ分だけ実体より大きい)
static size_t up_written;
static int last_pct = -1;
static uint8_t last_sta = 0xFF;
static bool wifi_mode = false;          // true=WiFi設定モード (戻れる)
static bool wifi_saved = false;         // WiFi設定モードで保存された
static bool routes_ready = false;       // server.on は1回だけ登録する

static const char PAGE_HEAD[] PROGMEM =
	"<!DOCTYPE html><html lang=\"ja\"><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>CW Decoder OTA</title><style>"
	"body{font-family:sans-serif;margin:20px;max-width:480px;color:#222}"
	"h1{font-size:20px}h2{font-size:16px;margin-top:28px}"
	"#bar{height:22px;background:#eee;border-radius:11px;overflow:hidden;margin:14px 0}"
	"#fill{height:100%;width:0;background:#2a7;transition:width .1s}"
	"button{padding:10px 22px;font-size:16px}#msg{font-weight:bold}"
	"input[type=text],input[type=password]{width:100%;padding:8px;font-size:16px;box-sizing:border-box}"
	"section{border-top:1px solid #ddd;padding-top:8px}"
	"</style></head><body>";

static const char PAGE_TAIL[] PROGMEM =
	"<script>function up(){var f=document.getElementById('f').files[0];"
	"if(!f){alert('firmware.bin を選択してください');return;}"
	"var fd=new FormData();fd.append('firmware',f,f.name);var x=new XMLHttpRequest();"
	"x.upload.onprogress=function(e){var p=Math.round(e.loaded/e.total*100);"
	"document.getElementById('fill').style.width=p+'%';"
	"document.getElementById('msg').textContent='送信中 '+p+'%';};"
	"x.onload=function(){document.getElementById('msg').textContent=x.responseText;};"
	"x.onerror=function(){document.getElementById('msg').textContent='通信エラー';};"
	"x.open('POST','/update');x.send(fd);}</script></body></html>";

//	----- 画面 -----

static void draw_status(const char *msg, uint16_t color)
{
	lcd->fillRect(0, STAT_Y, 320, 24, C_BG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(color, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
	lcd->drawString(msg, BAR_X, STAT_Y);
}

static void draw_bar(int pct)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	int w = (BAR_W - 4) * pct / 100;
	lcd->fillRect(BAR_X + 2, BAR_Y + 2, w, BAR_H - 4, C_OK);
	lcd->fillRect(BAR_X + 2 + w, BAR_Y + 2, (BAR_W - 4) - w, BAR_H - 4, C_BG);
}

static void draw_stations(uint8_t n)
{
	lcd->fillRect(0, INFO_Y, 200, 18, C_BG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(n ? C_OK : C_LABEL, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
	char buf[32];
	snprintf(buf, sizeof(buf), "接続端末 %u 台", n);
	lcd->drawString(buf, BAR_X, INFO_Y);
}

static void draw_screen(void)
{
	lcd->fillScreen(C_BG);

	lcd->fillRect(0, 0, 320, 30, C_TITLEBG);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_TITLETX, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_left);
	lcd->drawString(wifi_mode ? "WiFi設定" : "OTA モード", 8, 15);
	lcd->setFont(&fonts::DejaVu12);
	lcd->setTextColor(C_ACCENT, C_TITLEBG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_right);
	char ver[32];
	snprintf(ver, sizeof(ver), "v%s  %.11s", FW_VERSION, FW_BUILD);
	lcd->drawString(ver, 312, 15);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	// 左: 接続情報
	struct { const char *label; String value; } rows[] = {
		{ "SSID", String(OTA_AP_SSID) },
		{ "PASS", String(OTA_AP_PASS) },
		{ "URL",  String("http://") + WiFi.softAPIP().toString() + "/" },
	};
	int y = 38;
	for (auto &r : rows) {
		lcd->setFont(&fonts::DejaVu12);
		lcd->setTextColor(C_LABEL, C_BG);
		lcd->drawString(r.label, 8, y);
		lcd->setFont(&fonts::DejaVu18);
		lcd->setTextColor(C_VALUE, C_BG);
		lcd->drawString(r.value, 8, y + 14);
		y += 38;
	}
	draw_stations(0);

	// 右: WiFi接続用QR (スマホのカメラで読むとAPに参加できる)
	String wifi_qr = String("WIFI:T:WPA;S:") + OTA_AP_SSID + ";P:" + OTA_AP_PASS + ";;";
	lcd->qrcode(wifi_qr.c_str(), 206, 34, 106, 3);
	lcd->setFont(&fonts::DejaVu12);
	lcd->setTextColor(C_LABEL, C_BG);
	lcd->setTextDatum(lgfx::textdatum_t::top_center);
	lcd->drawString("scan: join & open page", 259, 144);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);

	// [キャンセル] は両モード共通 (右下)。OTA モードでは進捗バーをその左に置く
	lcd->fillRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 6, C_BTN_BG);
	lcd->drawRoundRect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 6, C_BTN_BD);
	lcd->setFont(&fonts::lgfxJapanGothicP_16);
	lcd->setTextColor(C_BTN_TX, C_BTN_BG);
	lcd->setTextDatum(lgfx::textdatum_t::middle_center);
	lcd->drawString("キャンセル", CANCEL_X + CANCEL_W / 2, CANCEL_Y + CANCEL_H / 2);
	lcd->setTextDatum(lgfx::textdatum_t::top_left);
	if (wifi_mode) {
		draw_status("ブラウザで SSID/パスワードを入力", C_ACCENT);
	} else {
		lcd->drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_FRAME);
		draw_bar(0);
		draw_status("待機中", C_ACCENT);
	}
}

//	----- HTTPハンドラ -----

//	自分の IP 以外のホスト名で来た要求 (OS の接続確認プローブ等) は
//	自分のページへ 302 で飛ばす。これがキャプティブポータル検出の合図になる
static bool redirect_if_captive(void)
{
	String host = server.hostHeader();
	String ip = WiFi.softAPIP().toString();
	if (host == ip || host == ip + ":80") {
		return false;
	}
	server.sendHeader("Location", String("http://") + ip + "/", true);
	server.send(302, "text/plain", "");
	return true;
}

static void handle_not_found(void)
{
	if (redirect_if_captive()) return;
	server.send(404, "text/plain", "not found");
}

static void handle_root(void)
{
	if (redirect_if_captive()) return;
	String page = FPSTR(PAGE_HEAD);
	page += "<h1>CW Decoder</h1>";
	page += "<p>現在のバージョン: <b>v" FW_VERSION "</b> (";
	page += FW_BUILD;
	page += ")</p>";

	if (wifi_mode) {
		// 1日1回の NTP 同期に使う自宅WiFiの登録 (本体にキーボードが無いのでここで入力する)
		page += "<section><h2>WiFi設定 (時刻同期用)</h2>";
		page += "<p>登録済み: <b>";
		page += netsync_has_wifi() ? netsync_ssid() : String("(未設定)");
		page += "</b></p><form method=\"POST\" action=\"/wifi\">";
		page += "<p>SSID<br><input type=\"text\" name=\"ssid\" value=\"";
		page += netsync_ssid();
		page += "\"></p>";
		page += "<p>パスワード<br><input type=\"password\" name=\"pass\"></p>";
		page += "<p><button type=\"submit\">保存</button></p></form>";
		page += "<p style=\"color:#666\">保存すると時計画面の表示中に1日1回だけWiFiを起動してNTP同期します。"
		        "保存後は本体が設定画面へ戻ります。</p></section>";
	} else {
		page += "<section><h2>ファーム更新</h2>";
		page += "<p><input type=\"file\" id=\"f\" accept=\".bin\"></p>";
		page += "<p><button onclick=\"up()\">アップロード</button></p>";
		page += "<div id=\"bar\"><div id=\"fill\"></div></div><p id=\"msg\"></p></section>";
	}

	page += FPSTR(PAGE_TAIL);
	server.send(200, "text/html; charset=utf-8", page);
}

static void handle_wifi(void)
{
	netsync_save_wifi(server.arg("ssid").c_str(), server.arg("pass").c_str());
	draw_status("WiFi設定を保存しました", C_OK);
	String page = FPSTR(PAGE_HEAD);
	page += "<h1>CW Decoder</h1><p>WiFi設定を保存しました: <b>";
	page += netsync_ssid();
	page += "</b></p><p>本体は設定画面へ戻ります。この接続 (CWDEC-OTA) は切れます。</p>";
	page += FPSTR(PAGE_TAIL);
	server.send(200, "text/html; charset=utf-8", page);
	wifi_saved = true;
}

static void handle_update_end(void)
{
	bool ok = !Update.hasError();
	server.sendHeader("Connection", "close");
	server.send(200, "text/plain; charset=utf-8",
	            ok ? "更新完了。本体が再起動します。" : "更新失敗。もう一度お試しください。");
	if (ok) {
		draw_bar(100);
		draw_status("完了 再起動します", C_OK);
		delay(1200);
		ESP.restart();
	}
}

static void handle_upload(void)
{
	HTTPUpload &up = server.upload();

	switch (up.status) {
	case UPLOAD_FILE_START:
		up_written = 0;
		last_pct = -1;
		up_total = server.header("Content-Length").toInt();
		Serial.printf("[ota] start: %s (content-length=%u)\n", up.filename.c_str(), (unsigned)up_total);
		draw_status("受信中 ...", C_ACCENT);
		// サイズ未確定のまま開始する。書き込み先は非アクティブ側の
		// アプリパーティションが自動選択される。
		if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
			Update.printError(Serial);
			draw_status("開始失敗 (領域不足?)", C_ERR);
		}
		break;

	case UPLOAD_FILE_WRITE:
		if (Update.write(up.buf, up.currentSize) != up.currentSize) {
			Update.printError(Serial);
			draw_status("書込エラー", C_ERR);
			break;
		}
		up_written += up.currentSize;
		if (up_total) {
			int pct = (int)(up_written * 100 / up_total);
			if (pct != last_pct) {
				last_pct = pct;
				draw_bar(pct);
				char buf[32];
				snprintf(buf, sizeof(buf), "受信中 %d%%", pct);
				draw_status(buf, C_ACCENT);
			}
		}
		break;

	case UPLOAD_FILE_END:
		if (Update.end(true)) {
			Serial.printf("[ota] success: %u bytes\n", (unsigned)up.totalSize);
		} else {
			Update.printError(Serial);
			draw_bar(0);
			draw_status("検証失敗", C_ERR);
		}
		break;

	default:    // UPLOAD_FILE_ABORTED
		Update.abort();
		Serial.println("[ota] aborted");
		draw_bar(0);
		draw_status("中断されました", C_ERR);
		break;
	}
}

//	----- エントリ -----

//	SoftAP と HTTP サーバを立ち上げて画面を描く (両モード共通)
static void ap_start(LGFX *lcd_)
{
	lcd = lcd_;
	last_sta = 0xFF;
	wifi_saved = false;

	Serial.printf("[ota] enter %s mode (heap=%u)\n", wifi_mode ? "wifi-setup" : "OTA",
	              (unsigned)ESP.getFreeHeap());
	WiFi.persistent(false);         // 資格情報をNVSに書かない
	WiFi.mode(WIFI_AP);
	WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS);
	delay(100);
	Serial.printf("[ota] AP ready: %s  ip=%s  heap=%u\n",
	              OTA_AP_SSID, WiFi.softAPIP().toString().c_str(), (unsigned)ESP.getFreeHeap());

	draw_screen();

	if (!routes_ready) {
		routes_ready = true;
		// アップロードハンドラ内で Content-Length を読むため明示的に収集する
		static const char *headers[] = { "Content-Length" };
		server.collectHeaders(headers, 1);
		server.on("/", HTTP_GET, handle_root);
		server.on("/wifi", HTTP_POST, handle_wifi);
		server.on("/update", HTTP_POST, handle_update_end, handle_upload);
		server.onNotFound(handle_not_found);
	}
	dns.setErrorReplyCode(DNSReplyCode::NoError);
	dns.start(53, "*", WiFi.softAPIP());
	server.begin();
}

//	接続台数の表示更新 (500ms毎)
static void ap_poll(void)
{
	static uint32_t t_last = 0;
	dns.processNextRequest();
	server.handleClient();
	if (millis() - t_last > 500) {
		t_last = millis();
		uint8_t n = WiFi.softAPgetStationNum();
		if (n != last_sta) {
			last_sta = n;
			draw_stations(n);
		}
	}
}

static void ap_stop(void)
{
	dns.stop();
	server.stop();
	// AP を止めてから WiFi を落とす。softAPdisconnect(true) だと内部で
	// 二重に deinit され "netstack cb reg failed" が出るので false にする
	WiFi.softAPdisconnect(false);
	WiFi.mode(WIFI_OFF);
	delay(50);
	Serial.printf("[ota] AP off, heap=%u\n", (unsigned)ESP.getFreeHeap());
}

//	AP モードのメインループ。戻り値: true=ユーザーがキャンセル / 保存で終了
//	(OTA が成功したときは再起動するので戻らない)
static bool ap_loop(void)
{
	int32_t tx, ty;
	while (lcd->getTouch(&tx, &ty)) {
		delay(10);                  // SETUP のタップが残っているうちは待つ
	}
	for (;;) {
		ap_poll();
		if (wifi_saved) {
			// 保存応答がブラウザへ届くまで少し待ってから AP を落とす
			uint32_t t0 = millis();
			while (millis() - t0 < 1500) {
				ap_poll();
				delay(2);
			}
			return true;
		}
		if (lcd->getTouch(&tx, &ty)) {
			Serial.printf("[ota] touch %d,%d\n", (int)tx, (int)ty);
			bool cancel = (tx >= CANCEL_X && tx < CANCEL_X + CANCEL_W &&
			               ty >= CANCEL_Y && ty < CANCEL_Y + CANCEL_H);
			while (lcd->getTouch(&tx, &ty)) {
				delay(10);
			}
			if (cancel) {
				if (Update.isRunning()) {
					Update.abort();
				}
				draw_status("キャンセル", C_LABEL);
				return true;
			}
		}
		delay(2);
	}
}

void ota_run(LGFX *lcd_)
{
	wifi_mode = false;
	ap_start(lcd_);
	ap_loop();
	ap_stop();
}

void wifi_setup_run(LGFX *lcd_)
{
	wifi_mode = true;
	ap_start(lcd_);
	ap_loop();
	ap_stop();
	wifi_mode = false;
}
