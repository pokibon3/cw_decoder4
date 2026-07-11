///////////////////////////////////////////////////////////////////////////
// CWデコーダ (Hjalmar Skovholm Hansen OZ1JHM) バージョン 1.01
// 自由に改変・複製できますが、GPL を遵守してください。
// ライセンス: <a href="http://www.gnu.org/copyleft/gpl.html"
// 		title="http://www.gnu.org/copyleft/gpl.html"
// 		rel="nofollow">http://www.gnu.org/copyleft/gpl.html</a>
// 議論・提案はこちら:
// <a href="https://groups.yahoo.com/neo/groups/oz1jhm/conversations/messages"
// 		title="https://groups.yahoo.com/neo/groups/oz1jhm/conversations/messages"
// 		rel="nofollow">https://groups.yahoo.com/neo/groups/oz1jhm/conversations/messages</a>
//
// KC2UEZ による改変 (バージョン 1.2):
// - Arduino NANO に対応
// - 起動時に「ターゲット周波数」と「帯域」を選択可能
///////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////
// Goertzel 法の解説: <a href="http://en.wikipedia.org/wiki/Goertzel_algorithm" 
// 		title="http://en.wikipedia.org/wiki/Goertzel_algorithm"
// 		rel="nofollow">http://en.wikipedia.org/wiki/Goertzel_algorithm</a>
// FFT の参考: <a href="http://www.dspguide.com/pdfbook.htm"
// 		title="http://www.dspguide.com/pdfbook.htm"
// 		rel="nofollow">http://www.dspguide.com/pdfbook.htm</a>
///////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////
//
//	CW Decoder Ver for UIAPduino Pro Micro
//
// 改変版 (Kimiwo Ohe)
// 変更点:
//  - UIAPduino Pro Micro(CH32V003) に移植
//  - 周波数検出機能を追加
//  - 全ソースをリファクタリング
// 日付: 2025-11-07 バージョン 1.0
//  - DFT を Goertzel 法に変更
// 日付: 2025.12.05 バージョン 1.1
//  - オーディオ入力ダイナミックレンジ拡大
// 日付: 2025.12.13 バージョン 1.2
//  - DFTの周波数を調整
//  - DFTの入力レベルを調整
// 日付: 2025.12.20 バージョン 1.3
//  - デコードタイミング微調整
// 日付: 2026.02.04 バージョン 1.4
//  - LCDに1.14inch ST7739をサポート
//　- 音声サンプリングのダブルバッファ化と表示期間の最適化
//  - ノイズブランカの改善
// 日付: 2026.02.07 バージョン 1.5
//  - ST7735/7789のドライバを新規作成し最適化
//  - 速度検出の安定化
//  - ノイズによる誤動作の軽減
// 日付: 2026.04.07 バージョン 1.6
//  - CH32V006 対応
//  - LCD/GPIO 配線差分に対応
//  - FLASH wait state 設定の修正
//  - ADC入力 PA2 を ADC_IN0 として修正
//  - CH32V006 の LED 制御を PC3 に修正
// 日付: 2026.04.10 バージョン 1.7
//  - CH32V006 の表示フォントを改善
//  - ST7735 は 12x16 ANK フォントを追加
//  - ST7789 は 15x21 ANK フォントを追加
//  - 2倍/3倍の単純拡大をやめ、006ではネイティブ字形を使用
//
// このソフトウェアは GNU General Public License (GPL) に基づき配布されています。
// 改変版も同じ GPL ライセンスで再配布してください。
///////////////////////////////////////////////////////////////////////////
//
//  Hardware Connections
//
//	UIAP	CH32V006  	    TFT(SPI)	SW		MIC    	 ETC.
//	10	    PC0             DC
//	 8      PC6            MOSI
//   9      PC7            RES
//   7      PC5            SCK
//   5      PA4            CS
//	A1	    PA1                         SW1
//	A2      PC4                         SW2
//  A3      PD2							SW3
//	A0		PA2									OUT(ADC_IN0)
//  A6		PD6                                 TEST
//  LED     PC3
//	3.3V                  3.3V           		VCC(Via 78L33 3.3V)
//  GND                    GND          		GND
//
///////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ch32fun.h"
#include "cw_decoder.h"
#include "frequencyDetector.h"

uint16_t sampling_period_us;
alignas(2) uint8_t  shared_buf[BUFSIZE];

alignas(4) float fft_real[SAMPLES];
alignas(4) float fft_imag[SAMPLES];

//==================================================================
//	main
//==================================================================
int main()
{
	SystemInit();				// MCU setup
	GPIO_setup();				// GPIO/ADC setup
    tft_init();					// LCD init

	while (1) {
		// cw decoder
		cwd_setup();			// freq detector Setup
		cwDecoder();			// run cw decoder
		// frequency detector
		fd_setup();				// freq detector Setup
		freqDetector(fft_real, fft_imag);			// run freq counter
	}
	return 0;
}
