//
//	Audio capture (ESP32 internal ADC via I2S DMA)
//
#pragma once
#include <stdint.h>
#include <stddef.h>

void audio_init(void);
// Reads n samples (blocking). Returns 12bit raw values (0..4095).
size_t audio_read(uint16_t *dst, size_t n);
