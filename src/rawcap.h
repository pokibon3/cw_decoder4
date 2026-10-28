//
//	診断: ADC 生サンプルの取り込み (ビルドフラグ RAWCAP=1 のときだけ入る)
//
//	実機に入っている音を PC で調べるためのもの (tools/rawcap.py)。2 通り:
//	  一括 ('r'): 4 点平均の前の 32kHz・12bit を RAWCAP_SAMPLES 個取り込んで出す
//	              (アナログ BPF の実特性や ADC の折り返しを見る)
//	  連続 ('s' で開始 / 'x' で停止): DSP に渡す 8kHz の値を流し続ける
//	              (そのまま cwcore.wasm に通せば、入力側と処理側を切り分けられる)
//	連続は約 240kbps 要るので、診断ビルドではシリアルを 460800bps にする
//	(main.cpp。921600 は macOS 標準の CH340 ドライバで化ける。
//	スコープビューアは 115200 固定なので繋がらない)。
//
//	  PLATFORMIO_BUILD_FLAGS=-DRAWCAP=1 pio run -e esp32dev_ili9341 -t upload
//
//	出力 (スコープログと同じポートに混ざる):
//	  R0 n=<個数> rate=<Hz> ms=<取り込み開始の millis>     一括の先頭
//	  R <3 桁 16 進 x 64 個>                     生の ADC 値 (0..4095)
//	  R1                                         一括の終わり
//	  Q0 rate=<Hz> ring=<個数>                   連続の開始
//	  Q <通し番号> <3 桁 16 進 x 64 個>          通し番号は先頭サンプルの番号 (飛びは欠落)
//	  Q1 total=<個数> dropped=<個数>             連続の終わり
//	  R! <理由>                                  バッファを確保できなかったとき
//
#pragma once
#include <stdint.h>

#ifndef RAWCAP
#define RAWCAP 0
#endif
#ifndef RAWCAP_SAMPLES
#define RAWCAP_SAMPLES 16384        // 32kHz で 0.51 秒、32KB (取れなければ半分ずつ減らす)
#endif
#define RAWCAP_BAUD 460800

#if RAWCAP
extern volatile uint8_t rawcap_state;   // 0=待機 1=一括取り込み中 2=一括送出待ち 3=連続
void rawcap_store(uint16_t v);          // audio_read から (DSP タスク)
void rawcap_stream(uint16_t v);
void rawcap_poll(void);                 // 表示ループから毎フレーム
#define RAWCAP_SAMPLE(v) do { if (rawcap_state == 1) rawcap_store(v); } while (0)
#define RAWCAP_DECIMATED(v) do { if (rawcap_state == 3) rawcap_stream(v); } while (0)
#else
#define RAWCAP_SAMPLE(v) do { } while (0)
#define RAWCAP_DECIMATED(v) do { } while (0)
#endif
