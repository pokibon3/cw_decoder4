//
//	CW Decoder Common header
//
#pragma once
#include <stdint.h>
#define GPIO_ADC_MUX_DELAY 100
#define GPIO_ADC_sampletime GPIO_ADC_sampletime_43cy

#define SCALE 6

#define micros() (SysTick->CNT / DELAY_US_TIME)
#define millis() (SysTick->CNT / DELAY_MS_TIME)

static const uint8_t GPIO_LOW = 0;
static const uint8_t GPIO_HIGH = 1;

// TFT: ST7789
#include "st7789.h"
#define tft_init st7789_init
#define tft_set_cursor st7789_set_cursor
#define tft_set_color st7789_set_color
#define tft_set_background_color st7789_set_background_color
#define tft_print_char st7789_print_char
#define tft_print st7789_print
#define tft_print_number st7789_print_number
#define tft_draw_pixel st7789_draw_pixel
#define tft_draw_line st7789_draw_line
#define tft_draw_rect st7789_draw_rect
#define tft_fill_rect st7789_fill_rect
#define tft_draw_bitmap st7789_draw_bitmap
#define TFT_WIDTH  ST7789_WIDTH
#define TFT_HEIGHT ST7789_HEIGHT
#define TFT_FONT_W 5
#define TFT_FONT_H 7
#define TFT_FONT_ADV 6

extern uint16_t adc_read_raw();
extern uint16_t adc_capture_u8(int8_t *dst, uint16_t samples, uint16_t sample_period_us);
extern void gpio_write_led(uint8_t level);
extern void gpio_write_test(uint8_t level);

#define TEST_HIGH gpio_write_test(GPIO_HIGH);
#define TEST_LOW  gpio_write_test(GPIO_LOW);

#define SAMPLES 128
#define BUFSIZE (SAMPLES * 2)

extern uint16_t sampling_period_us;

extern "C" int mini_snprintf(char* buffer, unsigned int buffer_len, const char *fmt, ...);
extern int GPIO_setup();
extern int check_input();

extern void tim1_pwm_init(void);
extern void tim1_pwm_stop(void);

extern "C" void TIM1_UP_IRQHandler(void) __attribute__((interrupt));
