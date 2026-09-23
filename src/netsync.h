//
//	1日1回の NTP 時刻合わせ
//
//	通常モードでは WiFi を止めておく方針なので、同期のときだけ
//	数秒間 STA で接続し、SNTP で時刻を取り込んだら即座に WiFi を落とす。
//	SSID/パスワードは OTAモードの Web ページから設定し、NVS に保存する。
//
//	同期中は無線ノイズがデコーダに入らないよう DSP を一時停止する
//	(dsp_set_paused)。呼び出しは時計画面表示中に限る (main.cpp)。
//
#pragma once
#include <Arduino.h>

#define NETSYNC_TZ_OFFSET (9 * 3600)        // JST

void netsync_init(void);
void netsync_poll(void);                    // loop から呼ぶ (期限が来たら同期)
bool netsync_sync_now(void);                // 即時同期 (WiFiを一時的にONにする)

bool netsync_has_wifi(void);
void netsync_save_wifi(const char *ssid, const char *pass);
void netsync_clear_wifi(void);              // WiFi初期化 (NVS の SSID/パスワードを消す)
String netsync_ssid(void);
String netsync_pass(void);                  // OTA が AP+STA で使う
uint32_t netsync_last_epoch(void);          // 最後に同期できた時刻 (0=未同期)

// 他のモジュール (fwupdate.cpp) が独自に NTP 同期できたときに呼ぶ。
// last_epoch を更新し、次回の日次同期予定を SYNC_INTERVAL_MS 後に
// 繰り延べる (起動直後に二重で WiFi が立ち上がらないようにする)
void netsync_note_synced(uint32_t epoch);
