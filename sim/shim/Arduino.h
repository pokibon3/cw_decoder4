//
//	Arduino / FreeRTOS / ESP-IDF スタブ (PC シミュレータ専用)
//
//	src/ のファームウェアのソースを Mac / Linux でそのままコンパイルする
//	ために、実際に使われているものだけをここで用意する。
//	実機と同じく DSP タスクは別スレッドで動くので、クリティカルセクションと
//	キューはミューテックスで本当に排他する (WASM 版の no-op とは違う)。
//	実体は sim/sim_main.cpp にある。
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <ctype.h>

unsigned long millis(void);
void delay(unsigned long ms);

//	ピン: BOOT ボタン (GPIO0) だけ。キーボードの B で押せる (sim_main.cpp)
#define LOW 0
#define HIGH 1
#define INPUT 0
#define INPUT_PULLUP 2
void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);

//	String: 表示と ota.cpp (Web ページ組み立て) に使う範囲だけ
class String {
public:
	String(const char *s = "") : s_(s ? s : "") {}
	String(const std::string &s) : s_(s) {}
	const char *c_str(void) const { return s_.c_str(); }
	size_t length(void) const { return s_.size(); }
	operator const char *() const { return s_.c_str(); }
	bool operator==(const char *o) const { return s_ == o; }
	bool operator==(const String &o) const { return s_ == o.s_; }
	String &operator+=(const String &o) { s_ += o.s_; return *this; }
	friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
	int indexOf(char c) const { size_t i = s_.find(c); return i == std::string::npos ? -1 : (int)i; }
	String substring(int from, int to = -1) const {
		return String(s_.substr((size_t)from, to < 0 ? std::string::npos : (size_t)(to - from)));
	}
	void toLowerCase(void) { for (auto &c : s_) c = (char)tolower((unsigned char)c); }
	long toInt(void) const { return atol(s_.c_str()); }
private:
	std::string s_;
};

#define PROGMEM
#define FPSTR(p) ((const char *)(p))

//	Serial: 標準出力へ出す。スコープログ (scopelog.cpp) は
//	availableForWrite() が 0 なので送らずに捨てる
struct SerialStub {
	void begin(unsigned long baud) { (void)baud; }
	void setTxBufferSize(size_t n) { (void)n; }
	int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void print(const char *s) { fputs(s, stdout); }
	void println(const char *s = "") { puts(s); }
	size_t write(const uint8_t *p, size_t n) { (void)p; return n; }
	size_t write(uint8_t c) { (void)c; return 1; }
	int availableForWrite(void) { return 0; }
	int available(void) { return 0; }
};
extern SerialStub Serial;

struct EspStub {
	unsigned getFreeHeap(void) { return 200 * 1024; }
	unsigned getMaxAllocHeap(void) { return 100 * 1024; }
	void restart(void);
};
extern EspStub ESP;

//	ヒープ: 実機は DMA 対応メモリから取るが PC では区別しない
#define MALLOC_CAP_DMA 0
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 100 * 1024; }

//==================================================================
//	FreeRTOS
//==================================================================
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

//	portMUX: 実体はミューテックス (DSP スレッドと表示スレッドの間)
struct portMUX_TYPE {
	void *m;
};
#define portMUX_INITIALIZER_UNLOCKED { nullptr }
void sim_mux_enter(portMUX_TYPE *mux);
void sim_mux_exit(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) sim_mux_enter(mux)
#define taskEXIT_CRITICAL(mux)  sim_mux_exit(mux)

void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                   uint32_t stack, void *arg, unsigned prio,
                                   void *handle, int core);

typedef struct sim_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned len, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
