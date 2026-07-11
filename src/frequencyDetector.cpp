//
//	Frequency Detector functions
//
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "frequencyDetector.h"
#include "ch32fun.h"

#include "float_fft.h"
#include <math.h>

#define FD_SAMPLING_FREQUENCY 6000	// Hz	+10%
#define FFT_FPS_MEASURE 0
#define FFT_PEAK_HOLD_FRAMES 20
#define FFT_SIGNAL_DETECT_THRESHOLD 7
#define FFT_VALUE_FONT_SCALE FONT_SCALE_24X24
#define FFT_VALUE_CHAR_ADV   (TFT_FONT_ADV * FONT_SCALE_24X24)
#define FFT_VALUE_AREA_HEIGHT 26

#define FFT_FRAME_HEIGHT    (TFT_HEIGHT - 8)
#define FFT_AREA_Y_TOP      ((TFT_HEIGHT * 19) / 80)
#define FFT_AREA_Y_BOTTOM   ((TFT_HEIGHT * 70) / 80)
#define FFT_AREA_HEIGHT     (FFT_AREA_Y_BOTTOM - FFT_AREA_Y_TOP + 1)
#define FFT_LABEL_Y         (FFT_FRAME_HEIGHT + 1)
#define FFT_Y_GAIN_NUM      3U
#define FFT_Y_GAIN_DEN      1U

static int check_sw()
{
    int ret = 0;
	int val = check_input();

	if (val == 1) {
		Delay_Ms(300);
        ret = 1;
	}

    return ret;
}

int fd_setup()
{
    tim1_pwm_stop();
    sampling_period_us = 900000L / FD_SAMPLING_FREQUENCY;

	tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, BLACK);
	tft_set_color(WHITE);

    return 0;
}

int freqDetector(float *vReal, float *vImag)
{
	uint16_t peakFrequency = 0;
	uint16_t oldFreequency = 0;
	uint8_t  oldHasSignal = 0;
	uint32_t lastSignalMs = 0;
	char buf[16];

	tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, BLACK);
	tft_draw_rect(0, 0, TFT_WIDTH, FFT_FRAME_HEIGHT, BLUE);

	uint8_t fft_bins = (uint8_t)(SAMPLES / 2);
	if (fft_bins > 64) fft_bins = 64;
	uint16_t bin_step = (TFT_WIDTH - 1) / fft_bins;
	if (bin_step < 3) bin_step = 3;
	uint16_t bar_width = 1;
	{
		const uint16_t usable_width = (TFT_WIDTH > 2) ? (uint16_t)(TFT_WIDTH - 2) : 0;
		const uint16_t target_bins = 59;
		if (fft_bins > target_bins) fft_bins = (uint8_t)target_bins;
		uint16_t w = (fft_bins > 0) ? (uint16_t)(usable_width / fft_bins) : 1;
		if (w < 1) w = 1;
		bar_width = (uint16_t)(w + 1);
		bin_step = (bar_width > 1) ? (bar_width - 1) : 1;
		{
			const uint8_t max_bins = (uint8_t)(usable_width / bin_step);
			if (max_bins < fft_bins) fft_bins = max_bins;
		}
	}
	uint16_t plot_width = bin_step * fft_bins;
	if (plot_width > (TFT_WIDTH - 2)) {
		plot_width = TFT_WIDTH - 2;
	}
	uint16_t plot_left = (TFT_WIDTH - plot_width) / 2;
	const uint16_t bin_hz = (uint16_t)(FD_SAMPLING_FREQUENCY / SAMPLES);
	uint16_t bin1 = (uint16_t)((1000 + (bin_hz / 2)) / bin_hz);
	uint16_t bin2 = (uint16_t)((2000 + (bin_hz / 2)) / bin_hz);
	if (bin1 >= fft_bins) bin1 = (fft_bins > 0) ? (uint16_t)(fft_bins - 1) : 0;
	if (bin2 >= fft_bins) bin2 = (fft_bins > 0) ? (uint16_t)(fft_bins - 1) : 0;
	if (bin1 == 0) bin1 = 1;
	if (bin2 == 0) bin2 = 1;
	const uint16_t bar_center_offset = (uint16_t)((bar_width > 0) ? ((bar_width - 1) / 2) : 0);
	uint16_t line1_x = plot_left + (bin1 * bin_step) + bar_center_offset;
	uint16_t line2_x = plot_left + (bin2 * bin_step) + bar_center_offset;
	uint16_t value_area_x = plot_left + (plot_width / 2);
	uint16_t value_area_right = (plot_left + plot_width > 2) ? (uint16_t)(plot_left + plot_width - 2) : value_area_x;
	uint16_t value_text_base = value_area_x + 2;

	tft_set_color(BLUE);
	const char* label0 = "0Hz";
	const char* label1 = "1KHz";
	const char* label2 = "2KHz";
	const uint8_t label_char_w = TFT_FONT_ADV;
	uint16_t label_w0 = (uint16_t)(strlen(label0) * label_char_w);
	uint16_t label_w1 = (uint16_t)(strlen(label1) * label_char_w);
	uint16_t label_w2 = (uint16_t)(strlen(label2) * label_char_w);
	uint16_t x0 = (plot_left > (label_w0 / 2)) ? (plot_left - (label_w0 / 2)) : 0;
	uint16_t x1 = (line1_x > (label_w1 / 2)) ? (line1_x - (label_w1 / 2)) : 0;
	uint16_t x2 = (line2_x > (label_w2 / 2)) ? (line2_x - (label_w2 / 2)) : 0;
	if (x0 + label_w0 > TFT_WIDTH) x0 = (uint16_t)(TFT_WIDTH - label_w0);
	if (x1 + label_w1 > TFT_WIDTH) x1 = (uint16_t)(TFT_WIDTH - label_w1);
	if (x2 + label_w2 > TFT_WIDTH) x2 = (uint16_t)(TFT_WIDTH - label_w2);
	tft_set_cursor(x0, FFT_LABEL_Y);
	tft_print(label0, FONT_SCALE_8X8);
	tft_set_cursor(x1, FFT_LABEL_Y);
	tft_print(label1, FONT_SCALE_8X8);
	tft_set_cursor(x2, FFT_LABEL_Y);
	tft_print(label2, FONT_SCALE_8X8);

	while (1) {
		uint16_t mag[SAMPLES / 2];
		uint32_t display_mag_q8[SAMPLES / 2];
#if FFT_FPS_MEASURE
		static uint32_t fps_last_ms = 0;
		static uint16_t fps_frames = 0;
		static uint16_t fps_value = 0;
#endif

		if (check_sw() == 1) {
            break;
        }

TEST_HIGH
		float fave = 0.0f;
		for (int i = 0; i < SAMPLES; i++) {
			uint32_t t = micros();
			uint16_t raw = adc_read_raw();
			fave += (float)raw;
			vReal[i] = (float)raw;
			while ((micros() - t) < sampling_period_us);
		}
TEST_LOW
		fave /= (float)SAMPLES;
		for (int i = 0; i < SAMPLES; i++) {
			vReal[i] = vReal[i] - fave;
			vImag[i] = 0.0f;
		}
		float_fft(vReal, vImag, 7);
		{
			const float norm = 1.0f / (float)SAMPLES;
			for (int i = 0; i < SAMPLES / 2; i++) {
				float m = sqrtf(vReal[i] * vReal[i] + vImag[i] * vImag[i]) * norm;
				mag[i] = (m > 65535.0f) ? 65535U : (uint16_t)m;
				display_mag_q8[i] = (uint32_t)mag[i] << 8;
			}
		}

		uint8_t maxIndex = 0;
		uint16_t maxValue = 0;
		static uint8_t bar_x[64];
		static uint8_t bar_h[64];
		static uint16_t bar_h_q8[64];
		static uint8_t peak_h[64];
		static uint8_t peak_ttl[64];
		static uint8_t linebuf[TFT_WIDTH * 2] = {0};
		const uint16_t line_width = (TFT_WIDTH > 2) ? (uint16_t)(TFT_WIDTH - 2) : 0;

		tft_fill_rect(1, FFT_AREA_Y_TOP, TFT_WIDTH - 2, FFT_AREA_HEIGHT, BLACK);

		for (int i = 1; i < fft_bins; i++) {
			uint16_t m = mag[i];
			uint32_t gain_num = FFT_Y_GAIN_NUM;
			uint32_t gain_den = FFT_Y_GAIN_DEN * 16U;
			uint32_t target_q8 = (display_mag_q8[i] * (uint32_t)SCALE * gain_num + (gain_den / 2U)) / gain_den;
			if (target_q8 >= bar_h_q8[i]) {
				bar_h_q8[i] = (uint16_t)(bar_h_q8[i] + (((target_q8 - bar_h_q8[i]) * 7U + 7U) / 8U));
			} else {
				bar_h_q8[i] = (uint16_t)(bar_h_q8[i] - (((bar_h_q8[i] - target_q8) * 3U + 3U) / 4U));
			}
			uint32_t h = (bar_h_q8[i] + 128U) >> 8;
			if (h == 0 && m > 0) h = 1;
			if (h > (uint16_t)(FFT_AREA_HEIGHT - 1)) h = (uint16_t)(FFT_AREA_HEIGHT - 1);
			bar_x[i] = (uint8_t)(plot_left + (i * bin_step));
			bar_h[i] = (uint8_t)h;
			if (h >= peak_h[i]) {
				peak_h[i] = (uint8_t)h;
				peak_ttl[i] = FFT_PEAK_HOLD_FRAMES;
			} else if (peak_ttl[i] > 0) {
				peak_ttl[i]--;
			} else {
				peak_h[i] = (uint8_t)h;
			}
			if (m > maxValue) {
				maxValue = m;
				maxIndex = i;
			}
		}
		for (int i = fft_bins; i < 64; i++) {
			bar_h_q8[i] = 0;
			peak_h[i] = 0;
			peak_ttl[i] = 0;
		}

		for (uint16_t y = FFT_AREA_Y_TOP; y <= FFT_AREA_Y_BOTTOM; y++) {
			uint16_t idx = 0;
			for (uint16_t x = 1; x < TFT_WIDTH - 1; x++) {
				linebuf[idx++] = 0;
				linebuf[idx++] = 0;
			}
			for (int i = 1; i < fft_bins; i++) {
				uint8_t h = bar_h[i];
				uint16_t x0 = bar_x[i];
				uint16_t x1 = (uint16_t)(x0 + bar_width - 1);

				if (h > 0) {
					uint16_t top = (uint16_t)(FFT_AREA_Y_BOTTOM - h);
					if (y == top || y == FFT_AREA_Y_BOTTOM) {
						for (uint16_t dx = 0; dx < bar_width; dx++) {
							uint16_t px = (uint16_t)(x0 + dx);
							if (px >= 1 && px < (TFT_WIDTH - 1)) {
								uint16_t off = (uint16_t)((px - 1) * 2);
								linebuf[off] = (uint8_t)(WHITE >> 8);
								linebuf[off + 1] = (uint8_t)WHITE;
							}
						}
					} else if (y > top && y < FFT_AREA_Y_BOTTOM) {
						if (x0 >= 1 && x0 < (TFT_WIDTH - 1)) {
							uint16_t off = (uint16_t)((x0 - 1) * 2);
							linebuf[off] = (uint8_t)(WHITE >> 8);
							linebuf[off + 1] = (uint8_t)WHITE;
						}
						if (x1 >= 1 && x1 < (TFT_WIDTH - 1)) {
							uint16_t off = (uint16_t)((x1 - 1) * 2);
							linebuf[off] = (uint8_t)(WHITE >> 8);
							linebuf[off + 1] = (uint8_t)WHITE;
						}
					}
				}

				uint8_t ph = peak_h[i];
				if (ph > 0) {
					uint16_t peak_y = (uint16_t)(FFT_AREA_Y_BOTTOM - ph);
					if (y == peak_y || (peak_y > FFT_AREA_Y_TOP && y + 1 == peak_y)) {
						for (uint16_t px = x0; px <= x1; px++) {
							if (px >= 1 && px < (TFT_WIDTH - 1)) {
								uint16_t off = (uint16_t)((px - 1) * 2);
								linebuf[off] = (uint8_t)(RED >> 8);
								linebuf[off + 1] = (uint8_t)RED;
							}
						}
					}
				}
			}
			if (line_width > 0) {
				tft_draw_bitmap(1, y, line_width, 1, linebuf);
			}
		}

		tft_draw_line(line1_x, 1, line1_x, FFT_FRAME_HEIGHT - 1, DARKBLUE);
		tft_draw_line(line2_x, 1, line2_x, FFT_FRAME_HEIGHT - 1, DARKBLUE);
		peakFrequency = (FD_SAMPLING_FREQUENCY / SAMPLES) * maxIndex;
		{
			uint32_t now = millis();
			uint8_t hasSignal = (maxValue >= FFT_SIGNAL_DETECT_THRESHOLD) ? 1 : 0;
			uint8_t showSignal = hasSignal;
			uint16_t displayFrequency = oldFreequency;

			if (hasSignal) {
				lastSignalMs = now;
				displayFrequency = peakFrequency;
			} else if (lastSignalMs != 0 && (now - lastSignalMs) < 1000U) {
				showSignal = 1;
			}

			if ((displayFrequency != oldFreequency) || (showSignal != oldHasSignal)) {
				if (showSignal) {
					mini_snprintf(buf, sizeof(buf), "%4dHz", displayFrequency + FD_SAMPLING_FREQUENCY / SAMPLES / 2);
				} else {
					strcpy(buf, "    Hz");
				}
				uint16_t value_cursor_x = (uint16_t)((6 - strlen(buf)) * FFT_VALUE_CHAR_ADV + value_text_base);
				{
					uint16_t text_w = (uint16_t)(strlen(buf) * FFT_VALUE_CHAR_ADV);
					if (value_area_right > text_w) {
						uint16_t right_aligned_x = (uint16_t)(value_area_right - text_w);
						if (right_aligned_x > value_text_base) {
							value_cursor_x = right_aligned_x;
						} else {
							value_cursor_x = value_text_base;
						}
					}
				}
				tft_set_cursor(value_cursor_x, 3);
				tft_set_color(YELLOW);
				tft_fill_rect(value_area_x, 1, value_area_right - value_area_x, FFT_VALUE_AREA_HEIGHT, BLACK);
				tft_print(buf, FFT_VALUE_FONT_SCALE);
				oldFreequency = displayFrequency;
				oldHasSignal = showSignal;
			}

		}
#if FFT_FPS_MEASURE
		fps_frames++;
		{
			uint32_t now = millis();
			if (fps_last_ms == 0) fps_last_ms = now;
			if ((now - fps_last_ms) >= 1000) {
				fps_value = fps_frames;
				fps_frames = 0;
				fps_last_ms = now;
				tft_set_color(GREEN);
				tft_fill_rect(2, 2, 6 * 8, 8, BLACK);
				tft_set_cursor(2, 2);
				tft_print("FPS:", FONT_SCALE_8X8);
				{
					char fps_buf[8];
					mini_snprintf(fps_buf, sizeof(fps_buf), "%u", fps_value);
					tft_print(fps_buf, FONT_SCALE_8X8);
				}
				tft_set_color(WHITE);
			}
		}
#endif
	}
	return 0;
}
