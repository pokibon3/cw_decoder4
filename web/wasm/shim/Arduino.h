//
//	Arduino / FreeRTOS スタブ (Web (WASM) ビルド専用)
//
//	src/dsp.cpp・decoder.cpp が #include <Arduino.h> しているので、
//	それらが実際に使っているものだけをここで供給する。
//	WASM は AudioWorklet の単一スレッドで動くので、クリティカル
//	セクションとタスク生成は何もしない実装でよい。
//
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// サンプル数由来の時刻 (cw_push() が進める)。DSP の診断出力でしか使われない
unsigned long millis(void);
void delay(unsigned long ms);

#ifdef __cplusplus
}
#endif

//	クリティカルセクション: シングルスレッドなので no-op
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux)  ((void)(mux))

//	タスク: dsp_start() が DSP タスクを生成するが、Web では
//	AudioWorklet が dsp_step() を直接駆動するので生成しない
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) ((void)(ticks))
static inline void xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                           unsigned stack, void *arg,
                                           unsigned prio, void *handle, int core)
{
	(void)fn; (void)name; (void)stack; (void)arg;
	(void)prio; (void)handle; (void)core;
}

//	Serial: 診断出力は DSP_DIAG / DEC_DIAG が 0 なので実体は使われない。
//	万一有効にされてもリンクが通るように空実装を持たせる
struct SerialStub {
	void printf(const char *fmt, ...) { (void)fmt; }
	void print(const char *s) { (void)s; }
	void println(const char *s = "") { (void)s; }
	void write(uint8_t c) { (void)c; }
	int available(void) { return 0; }
};
extern SerialStub Serial;

//	ESP.getFreeHeap(): dsp_set_paused() の診断出力でのみ使われる
struct EspStub {
	unsigned getFreeHeap(void) { return 0; }
};
extern EspStub ESP;
