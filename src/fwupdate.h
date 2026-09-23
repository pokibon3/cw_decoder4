//
//	起動時のオンラインアップデートチェック
//
//	スプラッシュ表示のあと、WiFi が登録済みで「自動更新」が ON なら
//	STA で数秒だけ接続し、NTP 同期と新しいファームウェアの確認を
//	1回の接続でまとめて行う。新版があれば本体の画面で確認を取り、
//	YES なら GitHub Pages から bin を取って書き換えて再起動する。
//
//	手動 OTA (SoftAP + ブラウザからアップロード, ota.cpp) はそのまま
//	残してあり、WiFi が無い場所ではそちらを使う。
//
//	呼び出しは ADC 連続 DMA (dsp_start) を始める前に限ること。
//	DMA を動かしたまま WiFi を起動すると WDT リセットになる。
//	起動時のこの位置なら DSP がまだ動いていないので、netsync のように
//	dsp_set_paused() で挟む必要がない。
//
#pragma once
#include <Arduino.h>
#include "lgfx_config.h"

void fwupdate_init(void);                   // NVS から ON/OFF を読む
bool fwupdate_check_enabled(void);          // 起動時チェックが ON か
void fwupdate_set_check_enabled(bool on);   // SETUP 画面のトグル

// 起動時チェック本体 (OFF / WiFi未登録なら何もせず即戻る)。
// 更新した場合は再起動するので戻らない。
//
// スプラッシュを出した直後に呼ぶこと: 状況はその最下段 (y 223..239) へ
// 書き足す。デコーダ画面への切り替えは呼び出し側が display_redraw() で行う
void fwupdate_check_on_boot(LGFX *lcd);
