//
// CW Decoder Common functions
//
#include <stdint.h>
#include "common.h"
#include "ch32fun.h"

#ifndef TIM_OC1M_2
#define TIM_OC1M_2 ((uint16_t)0x0040)
#endif
#ifndef TIM_OC1M_1
#define TIM_OC1M_1 ((uint16_t)0x0020)
#endif
#ifndef TIM_OC1PE
#define TIM_OC1PE ((uint16_t)0x0008)
#endif
#ifndef TIM_ARPE
#define TIM_ARPE ((uint16_t)0x0080)
#endif
#ifndef TIM_CC1E
#define TIM_CC1E ((uint16_t)0x0001)
#endif
#ifndef TIM_UG
#define TIM_UG ((uint16_t)0x0001)
#endif
#ifndef TIM_MOE
#define TIM_MOE ((uint16_t)0x8000)
#endif
#ifndef TIM_CEN
#define TIM_CEN ((uint16_t)0x0001)
#endif

// Pin mapping (UIAP board)
static const uint8_t SW1_PIN = 1; // PA1
static const uint8_t SW2_PIN = 4; // PC4
static const uint8_t SW3_PIN = 2; // PD2
static const uint8_t ADC_PIN = 2; // PA2 (ADC_IN0)
static const uint8_t LED_PIN = 3; // PC3
static const uint8_t ADC_CH_A2 = 0; // PA2 = ADC_IN0 on CH32V006
static const uint8_t UART_PIN = 5; // PD5
static const uint8_t TEST_PIN = 6; // PD6

// GPIO CFGLR nibble encodings (MODE[1:0] + CNF[1:0] << 2)
static const uint8_t GPIO_CFG_INPUT_ANALOG = 0x0;
static const uint8_t GPIO_CFG_INPUT_PUPD = 0x8;
static const uint8_t GPIO_CFG_OUTPUT_PP_10M = 0x1;
static const uint8_t GPIO_CFG_OUTPUT_AF_PP_10M = 0x9;

static inline void gpio_cfg_pin(GPIO_TypeDef *port, uint8_t pin, uint8_t cfg)
{
	uint32_t shift = (uint32_t)pin * 4U;
	uint32_t mask = (uint32_t)0xFU << shift;
	port->CFGLR = (port->CFGLR & ~mask) | ((uint32_t)cfg << shift);
}

static inline void gpio_write(GPIO_TypeDef *port, uint8_t pin, uint8_t level)
{
	if (level == GPIO_HIGH) {
		port->BSHR = (uint32_t)1U << pin;
	} else {
		port->BCR = (uint32_t)1U << pin;
	}
}

static inline uint8_t gpio_read(GPIO_TypeDef *port, uint8_t pin)
{
	return (uint8_t)((port->INDR >> pin) & 1U);
}

static void adc_init_ch0(void)
{
	// Match the working CH32V203 setup: configure ADC clock before enabling/resetting ADC.
	RCC->CFGR0 &= ~((uint32_t)0x1FU << 11);
	RCC->CFGR0 |= ((uint32_t)0x18U << 11);

	RCC->APB2PCENR |= RCC_APB2Periph_ADC1;
	RCC->APB2PRSTR |= RCC_APB2Periph_ADC1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_ADC1;

	// Use a moderate sample time on all channels to match the stable V003 behavior.
	ADC1->SAMPTR2 = (ADC_SMP0_1 << (3 * 0)) | (ADC_SMP0_1 << (3 * 1)) |
		(ADC_SMP0_1 << (3 * 2)) | (ADC_SMP0_1 << (3 * 3)) |
		(ADC_SMP0_1 << (3 * 4)) | (ADC_SMP0_1 << (3 * 5)) |
		(ADC_SMP0_1 << (3 * 6)) | (ADC_SMP0_1 << (3 * 7)) |
		(ADC_SMP0_1 << (3 * 8)) | (ADC_SMP0_1 << (3 * 9));
	ADC1->SAMPTR1 = (ADC_SMP0_1 << (3 * 0)) | (ADC_SMP0_1 << (3 * 1)) |
		(ADC_SMP0_1 << (3 * 2)) | (ADC_SMP0_1 << (3 * 3)) |
		(ADC_SMP0_1 << (3 * 4)) | (ADC_SMP0_1 << (3 * 5));

	ADC1->CTLR2 |= ADC_EXTSEL;
	ADC1->CTLR2 |= ADC_ADON;
	ADC1->CTLR2 |= CTLR2_RSTCAL_Set;
	while (ADC1->CTLR2 & CTLR2_RSTCAL_Set) {
	}
	ADC1->CTLR2 |= CTLR2_CAL_Set;
	while (ADC1->CTLR2 & CTLR2_CAL_Set) {
	}
}

static inline uint16_t adc_read_ch0_raw()
{
	ADC1->RSQR3 = ADC_CH_A2;
	ADC1->CTLR2 |= ADC_ADON;
	ADC1->CTLR2 |= ADC_SWSTART;
	while (!(ADC1->STATR & ADC_EOC)) {
	}
	return (uint16_t)ADC1->RDATAR;
}

void tim1_pwm_init(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR |= RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;

	TIM1->PSC = 0;
	TIM1->ATRLR = 5859 - 1;
	TIM1->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;
	TIM1->CTLR1 |= TIM_ARPE;
	TIM1->CCER |= TIM_CC1E;
	TIM1->SWEVGR |= TIM_UG;
	TIM1->INTFR = (uint16_t)~TIM_IT_Update;
	NVIC->IPRIOR[TIM1_UP_IRQn] = 0 << 7 | 1 << 6;
	NVIC->IENR[((uint32_t)(TIM1_UP_IRQn) >> 5)] |= (1U << ((uint32_t)(TIM1_UP_IRQn) & 0x1F));
	TIM1->DMAINTENR |= TIM_IT_Update;
	TIM1->CH1CVR = 128;
	TIM1->BDTR |= TIM_MOE;
	TIM1->CTLR1 |= TIM_CEN;
}

void tim1_pwm_stop(void)
{
	TIM1->DMAINTENR &= ~TIM_IT_Update;
	TIM1->CTLR1 &= ~TIM_CEN;
}

//==================================================================
// setup
//==================================================================
int GPIO_setup()
{
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
		RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO;

	// Switches: input pull-up.
	gpio_cfg_pin(GPIOA, SW1_PIN, GPIO_CFG_INPUT_PUPD);
	gpio_cfg_pin(GPIOC, SW2_PIN, GPIO_CFG_INPUT_PUPD);
	gpio_cfg_pin(GPIOD, SW3_PIN, GPIO_CFG_INPUT_PUPD);
	gpio_write(GPIOA, SW1_PIN, GPIO_HIGH);
	gpio_write(GPIOC, SW2_PIN, GPIO_HIGH);
	gpio_write(GPIOD, SW3_PIN, GPIO_HIGH);

	// ADC input: analog.
	gpio_cfg_pin(GPIOA, ADC_PIN, GPIO_CFG_INPUT_ANALOG);

	// TEST: output push-pull.
	gpio_cfg_pin(GPIOD, TEST_PIN, GPIO_CFG_OUTPUT_PP_10M);
	gpio_write(GPIOD, TEST_PIN, GPIO_LOW);

	gpio_cfg_pin(GPIOC, LED_PIN, GPIO_CFG_OUTPUT_PP_10M);
	gpio_write(GPIOC, LED_PIN, GPIO_LOW);

	// UART TX pin: alternate function push-pull.
	gpio_cfg_pin(GPIOD, UART_PIN, GPIO_CFG_OUTPUT_AF_PP_10M);

	adc_init_ch0();
	return 0;
}

//==================================================================
// check switch
//==================================================================
int check_input()
{
	if (!gpio_read(GPIOA, SW1_PIN)) {
		return 1;
	}
	if (!gpio_read(GPIOD, SW3_PIN)) {
		return 3;
	}
	if (!gpio_read(GPIOC, SW2_PIN)) {
		return 2;
	}
	return 0;
}

uint16_t adc_read_raw()
{
	return adc_read_ch0_raw();
}

uint16_t adc_capture_u8(int8_t *dst, uint16_t samples, uint16_t sample_period_us)
{
	uint32_t sum = 0;
	if (!dst || samples == 0) {
		return 0;
	}

	for (uint16_t i = 0; i < samples; i++) {
		uint32_t t = micros();
		uint8_t val = (uint8_t)((adc_read_ch0_raw() >> 2) & 0xFF);
		sum += val;
		dst[i] = (int8_t)val;
		while ((micros() - t) < sample_period_us) {
		}
	}
	return (uint16_t)(sum / samples);
}

void gpio_write_led(uint8_t level)
{
	gpio_write(GPIOC, LED_PIN, level);
}

void gpio_write_test(uint8_t level)
{
	gpio_write(GPIOD, TEST_PIN, level);
}
