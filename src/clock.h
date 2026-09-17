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
// 下段の世界時計をタップしたときの処理。ゾーンが変わったら true
bool clock_zone_touch(int32_t tx, int32_t ty);

// clock_redraw() の最後に呼ばれるフック (main 側のボタン再描画用)
void clock_set_redraw_hook(void (*fn)(void));

//	世界時計のゾーン (朝が早い順)。内部時刻は JST 基準で持ち、表示のときだけ
//	ゾーンのオフセットを足す。夏時間は SETUP の設定が ON のときだけ適用する
typedef struct {
	const char *code;       // アマチュア無線のプリフィクス表記
	int16_t std_min;        // 標準時オフセット (分)
	uint8_t dst_rule;       // CLOCK_DST_*
} clock_zone_t;

#define CLOCK_DST_NONE 0
#define CLOCK_DST_US   1        // 3月第2日曜 02:00 〜 11月第1日曜 02:00
#define CLOCK_DST_EU   2        // 3月最終日曜 01:00UTC 〜 10月最終日曜 01:00UTC
#define CLOCK_DST_NZ   3        // 9月最終日曜 02:00 〜 4月第1日曜 02:00
#define CLOCK_DST_AU   4        // 10月第1日曜 02:00 〜 4月第1日曜 02:00

#define CLOCK_ZONE_N 12
#define CLOCK_ZONE_COLS 6       // 6 列 x 2 行
#define CLOCK_ZONE_HOME 2       // JA (既定)
extern const clock_zone_t clock_zones[CLOCK_ZONE_N];

uint8_t clock_zone(void);                   // 選択中のゾーン
void clock_set_zone(uint8_t idx);
uint32_t clock_zone_now(uint8_t idx);       // そのゾーンのローカル時刻
void clock_set_zone_time(uint8_t idx, uint32_t local);  // 時刻合わせ (そのゾーンの時刻で)
uint8_t clock_summer_time(void);            // 夏時間を使うか (SETUP の設定)
void clock_set_summer_time(uint8_t on);     // NVS に保存する

uint32_t clock_now(void);           // 現在時刻 (1970-01-01 からの秒, JST)
void clock_set(uint32_t epoch);     // 時刻合わせ
void clock_break(uint32_t epoch, clock_tm_t *tm);
uint32_t clock_make(const clock_tm_t *tm);
uint8_t clock_days_in_month(int16_t year, uint8_t mon);
