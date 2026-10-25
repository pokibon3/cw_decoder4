//
//	WiFi スタブ (PC シミュレータ専用)
//	ota.cpp の WiFi設定 / OTA 画面を描くためだけのもの。電波は出さない。
//	AP の IP は実機と同じ 192.168.4.1、端末はつながってこない
//
#pragma once
#include <Arduino.h>

class IPAddress {
public:
	IPAddress(uint32_t a = 0) : a_(a) {}
	IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
		: a_((uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24)) {}
	String toString(void) const {
		char buf[16];
		snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a_ & 0xFF, (a_ >> 8) & 0xFF,
		         (a_ >> 16) & 0xFF, a_ >> 24);
		return String(buf);
	}
private:
	uint32_t a_;
};

typedef enum { WIFI_OFF = 0, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
typedef enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 } wl_status_t;

typedef enum {
	ARDUINO_EVENT_WIFI_AP_STACONNECTED,
	ARDUINO_EVENT_WIFI_AP_STADISCONNECTED,
	ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED,
} WiFiEvent_t;

typedef union {
	struct { int aid; } wifi_ap_staconnected;
	struct { int aid; int reason; } wifi_ap_stadisconnected;
	struct { struct { uint32_t addr; } ip; } wifi_ap_staipassigned;
} WiFiEventInfo_t;

class WiFiClass {
public:
	void onEvent(void (*fn)(WiFiEvent_t, WiFiEventInfo_t)) { (void)fn; }
	void persistent(bool p) { (void)p; }
	bool mode(wifi_mode_t m) { (void)m; return true; }
	bool softAP(const char *ssid, const char *pass) { (void)ssid; (void)pass; return true; }
	IPAddress softAPIP(void) { return IPAddress(192, 168, 4, 1); }
	uint8_t softAPgetStationNum(void) { return 0; }
	bool softAPdisconnect(bool off = false) { (void)off; return true; }
	bool setHostname(const char *h) { (void)h; return true; }
	wl_status_t begin(const char *ssid, const char *pass) { (void)ssid; (void)pass; return WL_DISCONNECTED; }
	wl_status_t status(void) { return WL_DISCONNECTED; }
	IPAddress localIP(void) { return IPAddress(); }
	bool disconnect(bool off = false, bool erase = false) { (void)off; (void)erase; return true; }
};
extern WiFiClass WiFi;
