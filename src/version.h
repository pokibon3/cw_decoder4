//
//	ファームウェアのバージョン / ビルド識別
//
//	FW_VERSION はリリース毎に手で上げる。FW_BUILD はコンパイル日時
//	(main.cpp で __DATE__ / __TIME__ から生成) で、同じバージョンの
//	ビルド違いを OTA 前後で見分けるために使う。
//
#pragma once

#define FW_VERSION "2.2.1"

extern const char FW_BUILD[];       // 例: "Sep 16 2026 12:34:56"
