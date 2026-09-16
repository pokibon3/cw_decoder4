//
//	OTA モード (SoftAP + ブラウザからの firmware.bin アップロード) と
//	WiFi 設定モード (同じ SoftAP で NTP 同期用の SSID/パスワードを登録)
//
//	通常動作中は WiFi を起動しない。ota_run() / wifi_setup_run() を
//	呼んだときだけ SoftAP と HTTP サーバを立ち上げる。
//	  ota_run        : 更新に成功すると再起動する。本体の [キャンセル] で戻る
//	  wifi_setup_run : 保存または本体の [キャンセル] で AP を落として戻る
//
#pragma once
#include "lgfx_config.h"

#define OTA_AP_SSID "CWDEC-OTA"
#define OTA_AP_PASS "cwdecoder"     // WPA2 は 8文字以上

void ota_run(LGFX *lcd);            // 成功時は再起動 / キャンセルで戻る
void wifi_setup_run(LGFX *lcd);     // 保存 / キャンセルで戻る
