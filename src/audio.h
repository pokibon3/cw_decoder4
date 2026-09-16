//
//	Audio capture (ESP32 ADC continuous DMA driver)
//
#pragma once
#include <stdint.h>
#include <stddef.h>

void audio_init(void);
// Reads n 8kHz samples (blocking). Each is the average of four 12bit
// conversions acquired at 32kHz.
size_t audio_read(uint16_t *dst, size_t n);
// Stops the ADC DMA and releases the driver. audio_init() may be called
// again afterwards to restart capture (used around WiFi activity).
void audio_stop(void);
