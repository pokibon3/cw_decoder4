//
//	ML デコーダ (DL 方式): ゲートの並び → 文字
//
//	従来のデコーダ (decoder.cpp) がゲートを開閉するたびに、終わった要素
//	(マーク / スペース) の長さを受け取り、GRU で文字に変える。従来の
//	デコーダは速度推定・ノイズブランカ・窓の選択のために動かし続け、
//	DL 方式のときは従来の文字の代わりにこちらの文字を出す。
//
//	モデルは web/tools/ml/train.py で学習し、export.py で mlweights.h に
//	書き出したもの。英文と和文 (ホレ〜ラタ) を読む。
//
#pragma once
#include <stdint.h>

void mldec_reset(void);
void mldec_set_emit(void (*emit)(uint8_t ch));
// 要素が終わったとき: mark=1 ならマーク、0 ならスペース。ms はその長さ
void mldec_element(uint8_t mark, uint32_t ms);
// スペースの途中 (ゲートが閉じている間) にブロックごとに呼ぶ。elapsed は
// スペースが始まってからの時間。送信の最後の文字はここで出る
void mldec_silence(uint32_t elapsed);
