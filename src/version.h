//
//	ファームウェアのバージョン / ビルド識別
//
//	FW_VERSION はリリース毎に手で上げる。FW_BUILD はコンパイル日時
//	(main.cpp で __DATE__ / __TIME__ から生成) で、同じバージョンの
//	ビルド違いを OTA 前後で見分けるために使う。
//
#pragma once

#define FW_VERSION "2.8"

extern const char FW_BUILD[];       // 例: "Sep 16 2026 12:34:56"

// ビルドしたパネル ("ST7789"/"ILI9341")。tools/make_release.sh が
// firmware_<FW_PANEL>.bin という名前で書き出すのに合わせてある。
// fwupdate.cpp が latest.txt からダウンロードする bin を選ぶのに使う
#if defined(PANEL_ST7789)
#define FW_PANEL "ST7789"
#else
#define FW_PANEL "ILI9341"
#endif
