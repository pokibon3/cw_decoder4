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

// スプライト共有バッファ (DMA対応メモリ、起動時に1回だけ確保)。
// デコーダ画面の3枚 (約60KB) と時計の3枚 (約89KB) は同時に使わないので
// 同じ領域を setBuffer で指し直して共有する。ヒープの断片化に依存せず、
// WiFi 使用中の空きメモリも 60KB 増える。
#define SPRITE_BYTES_16(w, h) (((size_t)(w) * (h) * 2 + 2 + 3) & ~(size_t)3)
#define SPRITE_ARENA_BYTES (3 * SPRITE_BYTES_16(122, 122))   // 時計3枚分が最大
void *display_sprite_arena(size_t need);   // need > SPRITE_ARENA_BYTES なら NULL

// デコーダ画面の表示/非表示。非表示中も受信文字はグリッドに溜め続け
// (描画だけ止める)、display_redraw() で最新状態を丸ごと描き直す。
void display_set_visible(uint8_t visible);
void display_redraw(void);
// 文字エリア中央付近をタップしたときのコールバック (時計画面へ切替用)
void display_set_center_tap(void (*fn)(void));
