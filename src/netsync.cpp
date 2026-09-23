//
//	1日1回の NTP 時刻合わせ
//
//	WiFi を上げてから落とすまでが1回あたり数秒〜20秒。その間は無線ノイズが
//	ADC に乗るので、デコーダ (DSPタスク) を一時停止して判定状態に
//	ノイズが入らないようにする。netsync_poll() は時計画面表示中にしか
//	呼ばれない (デコーダ画面では WiFi を一切起動しない)。
//
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "netsync.h"
#include "clock.h"
#include "dsp.h"

#define NVS_NS "cwdec"
#define SYNC_INTERVAL_MS (24UL * 60 * 60 * 1000)    // 成功したら次は24時間後
#define RETRY_INTERVAL_MS (60UL * 60 * 1000)        // 失敗したら1時間後
#define FIRST_SYNC_MS 15000UL                       // 起動15秒後に初回
#define WIFI_TIMEOUT_MS 15000UL
#define SNTP_TIMEOUT_MS 8000UL

#define C_OK   lgfx::color565(120, 220, 160)
#define C_WARN lgfx::color565(255, 208, 120)
#define C_ERR  lgfx::color565(240, 110, 100)
#define C_DIM  lgfx::color565(120, 140, 165)

static String ssid;
static String pass;
static uint32_t next_ms;
static uint32_t last_epoch;

void netsync_init(void)
{
	// 読み書きモードで開く: 初回起動でも名前空間が作られるので
	// 読み取り専用で開いたときの nvs_open NOT_FOUND エラーが出ない
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		// 未登録のキーを読むと Preferences がエラーを吐くので isKey で確認する
		if (prefs.isKey("ssid")) {
			ssid = prefs.getString("ssid", "");
			pass = prefs.getString("pass", "");
		}
		prefs.end();
	}
	next_ms = millis() + FIRST_SYNC_MS;
	Serial.printf("[sync] wifi %s\n", ssid.length() ? ssid.c_str() : "(未設定)");
}

bool netsync_has_wifi(void)
{
	return ssid.length() > 0;
}

String netsync_ssid(void)
{
	return ssid;
}

String netsync_pass(void)
{
	return pass;
}

uint32_t netsync_last_epoch(void)
{
	return last_epoch;
}

void netsync_note_synced(uint32_t epoch)
{
	last_epoch = epoch;
	next_ms = millis() + SYNC_INTERVAL_MS;
}

void netsync_clear_wifi(void)
{
	ssid = "";
	pass = "";
	last_epoch = 0;
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		prefs.clear();
		prefs.end();
	}
	Serial.println("[sync] wifi cleared");
}

void netsync_save_wifi(const char *new_ssid, const char *new_pass)
{
	ssid = new_ssid;
	pass = new_pass;
	Preferences prefs;
	if (prefs.begin(NVS_NS, false)) {
		prefs.putString("ssid", ssid);
		prefs.putString("pass", pass);
		prefs.end();
	}
	Serial.printf("[sync] wifi saved: %s\n", ssid.c_str());
}

bool netsync_sync_now(void)
{
	if (!netsync_has_wifi()) {
		clock_toast("WiFi未設定のため同期しません", C_DIM);
		return false;
	}

	clock_toast("時刻同期中 WiFi接続 ...", C_WARN);
	Serial.printf("[sync] connect to %s\n", ssid.c_str());

	dsp_set_paused(1);              // 無線ノイズをデコーダに入れない
	WiFi.persistent(false);
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid.c_str(), pass.c_str());

	uint32_t t0 = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
		delay(100);
	}

	bool ok = false;
	if (WiFi.status() == WL_CONNECTED) {
		clock_toast("時刻同期中 NTP問い合わせ ...", C_WARN);
		configTime(NETSYNC_TZ_OFFSET, 0, "ntp.nict.jp", "pool.ntp.org");
		struct tm ti;
		if (getLocalTime(&ti, SNTP_TIMEOUT_MS)) {
			clock_tm_t t;
			t.year = (int16_t)(ti.tm_year + 1900);
			t.mon = (uint8_t)(ti.tm_mon + 1);
			t.day = (uint8_t)ti.tm_mday;
			t.hour = (uint8_t)ti.tm_hour;
			t.min = (uint8_t)ti.tm_min;
			t.sec = (uint8_t)ti.tm_sec;
			t.wday = 0;
			last_epoch = clock_make(&t);
			clock_set(last_epoch);
			ok = true;
			Serial.printf("[sync] ok: %04d-%02d-%02d %02d:%02d:%02d\n",
			              t.year, t.mon, t.day, t.hour, t.min, t.sec);
		} else {
			Serial.println("[sync] sntp timeout");
		}
	} else {
		Serial.println("[sync] wifi connect failed");
	}

	// 同期が終わったら必ず WiFi を落とす (通常モードは無線OFFが原則)
	WiFi.disconnect(true, true);
	WiFi.mode(WIFI_OFF);
	delay(50);
	dsp_set_paused(0);
	Serial.printf("[sync] wifi off, heap=%u\n", (unsigned)ESP.getFreeHeap());

	next_ms = millis() + (ok ? SYNC_INTERVAL_MS : RETRY_INTERVAL_MS);

	if (ok) {
		clock_tm_t t;
		clock_break(last_epoch, &t);
		char buf[48];
		snprintf(buf, sizeof(buf), "NTP同期 %02d:%02d:%02d 済", t.hour, t.min, t.sec);
		clock_toast(buf, C_OK);
	} else {
		clock_toast("同期失敗 1時間後に再試行", C_ERR);
	}
	delay(1500);            // 結果を読ませてから時計へ戻す
	clock_redraw();
	return ok;
}

void netsync_poll(void)
{
	if (!netsync_has_wifi()) {
		return;
	}
	if ((int32_t)(millis() - next_ms) < 0) {
		return;
	}
	netsync_sync_now();
}
