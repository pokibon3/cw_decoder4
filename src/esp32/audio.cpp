//
//	Audio capture: ADC1_CH7 (GPIO35) sampled by I2S0 DMA.
//	ADC2 (e.g. GPIO27) is not usable here: the I2S built-in ADC path
//	supports ADC1 only, and ADC2 conflicts with WiFi.
//
#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include "audio.h"
#include "dsp.h"

#define AUDIO_I2S_PORT I2S_NUM_0

void audio_init(void)
{
	i2s_config_t cfg = {};
	cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
	cfg.sample_rate = DSP_SAMPLE_RATE;
	cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
	cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
	cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
	cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
	// DMAバッファは1個=48サンプル(6ms)にする。大きくすると i2s_read が
	// バッファ単位でまとめて返り、デコーダのマーク/スペース長測定が
	// バッファ長単位に量子化されてしまう。
	cfg.dma_buf_count = 8;
	cfg.dma_buf_len = 48;
	cfg.use_apll = false;

	i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, NULL);
	i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_7);
	adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);
	i2s_adc_enable(AUDIO_I2S_PORT);
}

size_t audio_read(uint16_t *dst, size_t n)
{
	size_t bytes_read = 0;
	i2s_read(AUDIO_I2S_PORT, dst, n * sizeof(uint16_t), &bytes_read, portMAX_DELAY);
	size_t cnt = bytes_read / sizeof(uint16_t);

	// The I2S-ADC path delivers each 32bit word with its two 16bit
	// samples swapped: un-swap pairs, then mask off the channel bits.
	for (size_t i = 0; i + 1 < cnt; i += 2) {
		uint16_t t = dst[i];
		dst[i] = dst[i + 1];
		dst[i + 1] = t;
	}
	for (size_t i = 0; i < cnt; i++) {
		dst[i] &= 0x0FFF;
	}
	return cnt;
}
