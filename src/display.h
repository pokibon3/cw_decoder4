//
//	Display (LovyanGFX / ST7789 320x240 landscape)
//
#pragma once
#include <stdint.h>

void display_init(void);
void display_splash(void);
void display_enqueue(uint8_t ch);   // decoder emit callback (thread-safe)
void display_update(void);          // call from loop (~30fps)
