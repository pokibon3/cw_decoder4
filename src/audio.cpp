//
//	Audio capture: ADC1_CH7 (GPIO35) sampled at 32kHz with the ESP-IDF
//	ADC continuous DMA driver, then decimated to 8kHz by four-point averaging.
//
#include <Arduino.h>
#include <esp_adc/adc_continuous.h>
#include <esp_err.h>
#include <soc/soc_caps.h>
#include "audio.h"
#include "dsp.h"

#define AUDIO_OVERSAMPLE 4
#define AUDIO_ADC_SAMPLE_RATE (DSP_SAMPLE_RATE * AUDIO_OVERSAMPLE)
#define AUDIO_FRAME_SAMPLES (DSP_HOP * AUDIO_OVERSAMPLE)
#define AUDIO_FRAME_BYTES (AUDIO_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define AUDIO_POOL_BYTES (AUDIO_FRAME_BYTES * 8)

static adc_continuous_handle_t adc_handle = nullptr;

void audio_init(void)
{
	// 192 conversions at 32kHz = 6ms, matching one 48-sample DSP hop.
	adc_continuous_handle_cfg_t handle_cfg = {};
	handle_cfg.max_store_buf_size = AUDIO_POOL_BYTES;
	handle_cfg.conv_frame_size = AUDIO_FRAME_BYTES;
	ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

	adc_digi_pattern_config_t pattern = {};
	pattern.atten = ADC_ATTEN_DB_12;
	pattern.channel = ADC_CHANNEL_7;
	pattern.unit = ADC_UNIT_1;
	pattern.bit_width = ADC_BITWIDTH_12;

	adc_continuous_config_t cfg = {};
	cfg.pattern_num = 1;
	cfg.adc_pattern = &pattern;
	cfg.sample_freq_hz = AUDIO_ADC_SAMPLE_RATE;
	cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
	cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
	ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cfg));
	ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

void audio_stop(void)
{
	if (!adc_handle) return;
	adc_continuous_stop(adc_handle);
	adc_continuous_deinit(adc_handle);
	adc_handle = nullptr;
}

size_t audio_read(uint16_t *dst, size_t n)
{
	static uint8_t frame[AUDIO_FRAME_BYTES];
	size_t produced = 0;
	uint32_t sum = 0;
	uint8_t sum_count = 0;

	while (produced < n) {
		uint32_t bytes_read = 0;
		esp_err_t err = adc_continuous_read(adc_handle, frame, sizeof(frame),
		                                    &bytes_read, ADC_MAX_DELAY);
		if (err != ESP_OK) {
			return produced;
		}

		for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= bytes_read;
		     i += SOC_ADC_DIGI_RESULT_BYTES) {
			const adc_digi_output_data_t *result =
				(const adc_digi_output_data_t *)&frame[i];
			if (result->type1.channel != ADC_CHANNEL_7) continue;
			sum += result->type1.data;
			if (++sum_count == AUDIO_OVERSAMPLE) {
				dst[produced++] = (uint16_t)((sum + AUDIO_OVERSAMPLE / 2) /
				                             AUDIO_OVERSAMPLE);
				sum = 0;
				sum_count = 0;
				if (produced == n) break;
			}
		}
	}
	return produced;
}
