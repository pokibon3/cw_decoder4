//
//	スコープログ: スコープ画面の元データをシリアルへテキストで流す
//	(Web アプリで画面を再現するため)。SETUP の「スコープログ」で ON/OFF。
//
//	送出は表示ループ (Core 1) からだけ行い、DSP タスクは一切書かない。
//	TX バッファに入り切らない行は捨てて欠落数を報告する (ブロックしない)。
//
//	行フォーマット (1 行 1 レコード、フィールドは空白区切り):
//	  H rate=8000 hop=59 cols=150 ver=2.1      開始時と 5 秒ごと
//	  T <col> wpm=<n> tone=<hz> auto=<0/1> hopq8=<q8>   状態変化時 (掃引=hopq8/256 hop/列)
//	  S <col> <t_ms> <min> <max> <env> <gate> スコープ列ごと (t_ms はサンプル数由来)
//	  C <col> <文字 UTF-8>                     デコード文字 (符号区間中央の列)
//	  L <peak> <clip>                          入力ピーク (カウント) と累積クリップ数、毎秒
//	  X dropped=<n>                            欠落列数 (欠落があったときだけ)
//
#pragma once
#include <stdint.h>

void scopelog_set_enabled(uint8_t on);
uint8_t scopelog_enabled(void);
void scopelog_poll(void);               // 表示ループから毎フレーム呼ぶ
// デコード文字の記録 (display_enqueue から。DSP タスク文脈、書き込みのみ)
void scopelog_char(uint8_t ch, uint32_t col);
