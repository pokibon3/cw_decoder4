//
//	DSP: 3ビンGoertzelトーン検出 (v1.9同等) + 表示用FFT/スコープデータ生成
//
#pragma once
#include <stdint.h>

#define DSP_SAMPLE_RATE 8000
// トーン判定ブロック長 (ホップ)。59 サンプル = 7.375ms。
// v2.0 のノイズ対策は ADC が実効 6.5kHz で動いていた状態で実機調整されており、
// そのときの物理条件 (ブロック 7.33ms / 窓 68Hz / サイド ±273Hz) を
// 正しい 8kHz のもとで再現する値。48 (6ms) では時定数が 18% 短く、
// 窓とサイドが 22% 広くなってノイズ耐性が落ちる (実機で確認)
#define DSP_HOP 59
#define DSP_SPEC_N 256          // 表示用FFT (31.25Hz/bin)
#define DSP_SPEC_LOG2 8
#define DSP_SPEC_BINS 64        // 表示 0〜2kHz

typedef struct {
	int16_t mn;                 // 生波形 min (DC除去後)
	int16_t mx;                 // 生波形 max
	uint16_t mag;               // トーンエンベロープ (デコーダ正規化後)
	uint8_t gate;               // デコーダのキー判定 (0/1)
} scope_col_t;

#define DSP_TONE_COUNT 5            // 600/700/800/900/1000Hz
#define DSP_TONE_AUTO DSP_TONE_COUNT // AUTO: 600〜1000Hzの最強ピークへ自動同調
#define DSP_GATE_WIN (DSP_HOP * 2)  // トーン判定Goertzel窓 118 (14.75ms、帯域67.8Hz)

void dsp_start(void);
// 一時停止: DSPタスクを待機させ ADC DMA を止める (再開時に初期化し直す)。
// WiFi を使う間 (NTP同期 / WiFi設定) は必ず止めること: DMA を動かしたまま
// WiFi を起動すると割り込みウォッチドッグでリセットされる。
// 無線ノイズがデコーダ状態に入らない効果も兼ねる
void dsp_set_paused(uint8_t paused);
void dsp_set_tone(uint8_t idx);     // 0..4=手動 / DSP_TONE_AUTO=自動
uint8_t dsp_tone_index(void);
uint8_t dsp_tone_is_auto(void);
uint16_t dsp_tone_hz(void);         // 現在のゲート中心周波数 (AUTO時は追従値)
uint16_t dsp_tone_hz_at(uint8_t idx);
// 直近 n カラム分を out[0]=最古 .. out[n-1]=最新 でコピー
int dsp_get_scope(scope_col_t *out, int n);
// スコープ1列の時間 (0.1ms単位)。掃引はWPM追従で可変
uint16_t dsp_scope_col_ms_x10(void);
// 現在のトーン判定帯域幅 (Hz)。窓長のWPM追従で83/167が切り替わる
uint16_t dsp_gate_bw_hz(void);
// 入力レベル (ADCフルスケール比 0〜100%)。100%付近はクリップの目安
uint8_t dsp_input_level_pct(void);
// DSP_SPEC_BINS+1 個 (bin 0..64) をコピー
void dsp_get_spectrum(uint16_t *out);
uint16_t dsp_peak_hz(void);         // 検出ピーク周波数 (無信号時 0)
