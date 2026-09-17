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
// ESP32 の ADC continuous (I2S 経由) は sample_freq_hz に対して実効レートが
// 9/11 倍になる (実測: 要求 20k/32k/40k/64k → 16.36k/26.18k/32.72k/52.36k、
// audio_rate_sweep() で再測定可)。要求値を 11/9 倍して実効 32kHz に合わせる。
#define AUDIO_ADC_RATE_REQ ((uint32_t)AUDIO_ADC_SAMPLE_RATE * 11 / 9)
#define AUDIO_FRAME_SAMPLES (DSP_HOP * AUDIO_OVERSAMPLE)
#define AUDIO_FRAME_BYTES (AUDIO_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define AUDIO_POOL_BYTES (AUDIO_FRAME_BYTES * 8)

static adc_continuous_handle_t adc_handle = nullptr;
static uint32_t audio_req_rate = AUDIO_ADC_RATE_REQ;      // 診断掃引で差し替える
static volatile uint32_t audio_clips = 0;   // ADC レール到達 (0 / 4095 付近) の変換数

void audio_init(void)
{
	// DSP_HOP*4 conversions at 32kHz = one DSP hop (30 samples = 3.75ms).
	// conv_frame_size must be a multiple of SOC_ADC_DIGI_DATA_BYTES_PER_CONV (4).
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
	cfg.sample_freq_hz = audio_req_rate;
	cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
	cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
	ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cfg));
	ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

uint32_t audio_clip_total(void)
{
	return audio_clips;
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
			uint16_t v = result->type1.data;
			if (v <= 4 || v >= 4090) audio_clips++;   // ハードクリップ検出
			sum += v;
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

#if AUDIO_RATE_DIAG
//	診断: 要求レートを変えながら実効変換レートを測る。
//	main の setup() で audio_init() の前に呼ぶ (3秒 x 9レート)。
//	結果は要求 x 9/11 が実効レート (AUDIO_ADC_RATE_REQ の根拠)
void audio_rate_sweep(void)
{
	static const uint32_t rates[] = { 20000, 24000, 32000, 36000, 38930, 40000, 44100, 48000, 64000 };
	static uint8_t frame[AUDIO_FRAME_BYTES];
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		audio_req_rate = rates[i];
		audio_init();
		uint32_t conv = 0;
		uint32_t t0 = millis();
		// 最初の1フレームは捨てて起動遅れを除く
		uint32_t br = 0;
		adc_continuous_read(adc_handle, frame, sizeof(frame), &br, 1000);
		t0 = millis();
		while (millis() - t0 < 3000) {
			br = 0;
			if (adc_continuous_read(adc_handle, frame, sizeof(frame), &br, 1000) == ESP_OK) {
				conv += br / SOC_ADC_DIGI_RESULT_BYTES;
			}
		}
		uint32_t dt = millis() - t0;
		Serial.printf("[adc] req=%u -> actual=%u Hz (conv=%u in %u ms)\n",
		              (unsigned)rates[i], (unsigned)(conv * 1000ULL / dt),
		              (unsigned)conv, (unsigned)dt);
		audio_stop();
	}
	audio_req_rate = AUDIO_ADC_RATE_REQ;
}
#endif
