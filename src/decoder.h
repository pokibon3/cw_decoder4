//
//	CW decode state machine (v1.9 logic, block-driven)
//
#pragma once
#include <stdint.h>

void decoder_init(void);
void decoder_set_emit(void (*emit)(uint8_t ch));
// Call once per gate block (DSP_HOP samples, ~7.4ms) with the decoder-normalized magnitudes.
// side_mag = min(EMA(L), EMA(H)), side_mag_inst = min(L, H) of current block,
// side_mag_max = max(EMA(L), EMA(H))
// near_side = min(EMA) of +-1 hop-bin (+-136Hz) (band-limited-noise detector),
// jitter = EMA of |dMag| between blocks (envelope stability)
// tone_mag = 純音判定 (サイド比) に使う中心マグニチュード。通常は magnitude と同じ
void decoder_process_block(int32_t magnitude, int32_t tone_mag, int32_t side_mag,
                           int32_t side_mag_inst, int32_t side_mag_max, int32_t near_side,
                           int32_t jitter);
// ALE 判定用: ON/OFF を外で決めて渡し、タイミング処理だけを使う (評価中)
void decoder_process_key(int32_t magnitude, int32_t limit, uint8_t tone_on, uint8_t tone_off);
uint16_t decoder_wpm(void);
uint8_t decoder_gate(void);       // filtered key state (0/1)
int32_t decoder_maglimit(void);   // 適応振幅しきい値 (診断用)
int32_t decoder_noise_floor(void); // ゲートOFF中のノイズ床推定 (診断用)
// 直近ブロックのトーン判定結果 (診断用): bit0=tone_on, bit1=tone_off,
// bit2=realstate。列単位のログでは再現できないブロック単位の実際の判定
uint8_t decoder_gate_flags(void);
uint8_t decoder_mode(void);       // MODE_US / MODE_JP
void decoder_toggle_mode(void);
