//
//	セットアップ画面 (メニュー)
//
//	  ・時刻合わせ            -> timeset 画面へ
//	  ・ファームウェアアップデート -> 確認のうえ OTAモードへ (戻らない)
//	  ・WiFi初期化            -> 確認のうえ NVS の SSID/パスワードを消す
//	  下段にバージョン等のステータスを表示する。
//
#pragma once
#include "lgfx_config.h"

void setup_run(LGFX *lcd);      // 「戻る」が押されると復帰する
