//
//	Display (LovyanGFX / ST7789 320x240 landscape)
//
#pragma once
#include <stdint.h>
#include "lgfx_config.h"

void display_init(void);
// 絵柄を描いて 1.5 秒見せる (画面はそのまま。デコーダ画面への切り替えは
// 起動時アップデートチェックの後に呼び出し側が display_redraw() で行う)
void display_splash(void);
// スプラッシュの絵柄だけを描く (待ちも画面切替もしない)。最下段 (y 223..239) は
// 空けてあり、起動時アップデートチェックが状況をそこに書く (fwupdate.cpp)
void display_splash_draw(void);
// スプラッシュの見出し (タイトル + モールス飾り + 罫線) を title_y に描く。
// About 画面がスプラッシュと同じ体裁を使うために公開している
void display_splash_header(int title_y);
#define DISPLAY_SPLASH_BG lgfx::color565(4, 10, 24)

// 確認ダイアログ (title / 2行の説明 / キャンセル・ok_label)。
// SETUP 画面のほか、起動時アップデートチェックからも使う共通部品
bool display_confirm(LGFX *lcd, const char *title, const char *line1, const char *line2,
                     const char *ok_label);
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
// ステータス行の SETUP ボタンをタップしたときのコールバック
void display_set_setup_tap(void (*fn)(void));
