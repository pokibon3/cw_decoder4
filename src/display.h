//
//	Display (LovyanGFX / ST7789 320x240 landscape)
//
#pragma once
#include <stdint.h>
#include "lgfx_config.h"

void display_init(void);
void display_splash(void);
void display_enqueue(uint8_t ch);   // decoder emit callback (thread-safe)
void display_update(void);          // call from loop (~30fps)

LGFX *display_lcd(void);            // 他画面 (時計/セットアップ/OTA) が使う LCD

// デコーダ画面の表示/非表示。非表示中も受信文字はグリッドに溜め続け
// (描画だけ止める)、display_redraw() で最新状態を丸ごと描き直す。
void display_set_visible(uint8_t visible);
void display_redraw(void);
// 文字エリア中央付近をタップしたときのコールバック (時計画面へ切替用)
void display_set_center_tap(void (*fn)(void));
