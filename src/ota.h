//
//	OTA モード (SoftAP + ブラウザからの firmware.bin アップロード) と
//	WiFi 設定モード (同じ SoftAP で NTP 同期用の SSID/パスワードを登録)
//
//	通常動作中は WiFi を起動しない。ota_run() / wifi_setup_run() を
//	呼んだときだけ SoftAP と HTTP サーバを立ち上げる。
//	自宅WiFiが登録されていれば STA も同時に上げる (AP+STA) ので、
//	PC を自宅WiFiにつないだまま http://cwdec.local/ で更新できる。
//	  ota_run        : 更新に成功すると再起動する。本体の [キャンセル] で戻る
//	  wifi_setup_run : 保存または本体の [キャンセル] で AP を落として戻る
//
#pragma once
#include "lgfx_config.h"

#define OTA_AP_SSID "CWDEC-OTA"
#define OTA_AP_PASS "cwdecoder"     // WPA2 は 8文字以上
// 自宅WiFiが登録済みなら AP と同時に STA でも接続し、mDNS でこの名前を配る。
// ネットワークを切り替えずに http://cwdec.local/ で更新できる
#define OTA_HOSTNAME "cwdec"

void ota_run(LGFX *lcd);            // 成功時は再起動 / キャンセルで戻る
void wifi_setup_run(LGFX *lcd);     // 保存 / キャンセルで戻る
