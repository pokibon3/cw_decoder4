//
//	CW decode state machine (v1.9 logic, block-driven)
//
#pragma once
#include <stdint.h>

void decoder_init(void);
void decoder_set_emit(void (*emit)(uint8_t ch));
// Call once per gate block (~6ms) with the decoder-normalized magnitudes.
void decoder_process_block(int32_t magnitude, int32_t side_mag, int32_t side_mag_inst);
uint16_t decoder_wpm(void);
uint8_t decoder_gate(void);       // filtered key state (0/1)
int32_t decoder_maglimit(void);   // 適応振幅しきい値 (診断用)
uint8_t decoder_mode(void);       // MODE_US / MODE_JP
void decoder_toggle_mode(void);
