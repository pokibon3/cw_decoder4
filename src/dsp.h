//
//	DSP: 3ビンGoertzelトーン検出 (v1.9同等) + 表示用FFT/スコープデータ生成
//
#pragma once
#include <stdint.h>

#define DSP_SAMPLE_RATE 8000
#define DSP_HOP 48              // 6ms: トーン判定ブロック長 (Goertzel窓)
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
#define DSP_GATE_WIN 96             // トーン判定Goertzel窓 (12ms、帯域83.3Hz)

void dsp_start(void);
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
// DSP_SPEC_BINS+1 個 (bin 0..64) をコピー
void dsp_get_spectrum(uint16_t *out);
uint16_t dsp_peak_hz(void);         // 検出ピーク周波数 (無信号時 0)
