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
//	  S <col> <t_ms> <min> <max> <env> <gate> <side> <near> <limit> <nf>
//	                                           スコープ列ごと (t_ms はサンプル数由来)。
//	                                           side/near/limit/nf はトーン判定に使われた値で、
//	                                           ON: env > 0.6*limit かつ env > side*3 かつ env > near*2
//	                                           OFF: env < 0.4*limit または env < side*2.5 または env < near*1.5
//	  C <col> <文字 UTF-8>                     デコード文字 (符号区間中央の列)
//	  W <col>                                  語間 (スペース)
//	  E <col> <M|S> <ms> <unit>                要素 (マーク/スペース) の実測長と
//	                                           そのときの短点長推定 (デコーダ内部値)
//	  L <peak> <clip>                          入力ピーク (カウント) と累積クリップ数、毎秒
//	  X dropped=<n>                            欠落列数 (欠落があったときだけ)
//
#pragma once
#include <stdint.h>

void scopelog_set_enabled(uint8_t on);
uint8_t scopelog_enabled(void);
void scopelog_poll(void);               // 表示ループから毎フレーム呼ぶ
// デコード文字の記録 (display_enqueue から。DSP タスク文脈、書き込みのみ)。
// ch == ' ' は語間として W レコードで出す
void scopelog_char(uint8_t ch, uint32_t col);
// 要素 (マーク/スペース) が確定したときに decoder から呼ぶ。
// mark=1 でマーク、0 でスペース。ms は実測長、unit は短点長の推定値。
// スコープの KEY 列は 1 列 = 数 hop の多数決で量子化されるので、
// タイミングの検証にはこちらの値を使う
void scopelog_element(uint8_t mark, uint32_t ms, uint32_t unit);
