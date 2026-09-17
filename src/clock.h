//
//	通常モード画面: 昔ながらのパタパタ (split-flap) 時計
//
//	上段に西暦からの日付 (日本語ゴシック)、その下に 時 / 分 / 秒 の3枚のカード。
//	桁が変わるときだけ上フラップが倒れて下フラップが起き上がる動きを描く。
//
//	RTC が無いので時刻は「基準時刻 + 起動からの経過」で進める。
//	基準時刻はビルド日時で初期化し、
//	  - セットアップ画面 (手動)
//	  - 1日1回の NTP 同期 (netsync)
//	で合わせ直す。
//
#pragma once
#include "lgfx_config.h"

typedef struct {
	int16_t year;       // 西暦
	uint8_t mon;        // 1..12
	uint8_t day;        // 1..31
	uint8_t hour;       // 0..23
	uint8_t min;        // 0..59
	uint8_t sec;        // 0..59
	uint8_t wday;       // 0=日 .. 6=土
} clock_tm_t;

void clock_init(LGFX *lcd);         // 時刻基準の初期化のみ (描画しない)
void clock_update(void);            // loop から毎周期呼ぶ
void clock_free(void);              // 描画停止 (バッファはデコーダ画面と共有なので解放しない)
bool clock_alloc(void);             // 時計画面に入るたびに呼ぶ: 共有バッファへ割り付け
void clock_redraw(void);            // 他画面から戻ったときの再描画
void clock_toast(const char *msg, uint16_t color);      // 画面中央に一時メッセージ

// clock_redraw() の最後に呼ばれるフック (main 側のボタン再描画用)
void clock_set_redraw_hook(void (*fn)(void));

uint32_t clock_now(void);           // 現在時刻 (1970-01-01 からの秒, JST)
void clock_set(uint32_t epoch);     // 時刻合わせ
void clock_break(uint32_t epoch, clock_tm_t *tm);
uint32_t clock_make(const clock_tm_t *tm);
uint8_t clock_days_in_month(int16_t year, uint8_t mon);
