//
//	タッチパネルの校正 (4隅タップ) と、その値の保存 / 復元
//
//	XPT2046 の生の AD 値と画面座標の対応はボード個体差が大きく、
//	lgfx_config.h の x_min/x_max/y_min/y_max は「だいたい合う」既定値でしかない。
//	ズレると時計画面の下段ボタンのような小さな的が押せなくなるので、
//	SETUP から実機で校正し、結果を NVS ("cwdec"/"touchcal") に残す。
//
//	保存するのは LovyanGFX の calibrateTouch() が返す uint16_t[8]
//	(4隅の生 AD 値)。起動時に touchcal_load() で setTouchCalibrate() へ渡す。
//
#pragma once
#include "lgfx_config.h"

bool touchcal_saved(void);          // 校正値が NVS にあるか
bool touchcal_load(LGFX *lcd);      // あれば適用する (起動時に一度)
void touchcal_reset(LGFX *lcd);     // 校正値を消して既定値へ戻す
void touchcal_run(LGFX *lcd);       // SETUP から呼ぶ校正画面 (完了で保存)
