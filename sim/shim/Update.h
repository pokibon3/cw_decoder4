//	Update (OTA の書き込み) スタブ (PC シミュレータ専用)。何も書かない
#pragma once
#include <Arduino.h>

#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF

class UpdateClass {
public:
	bool begin(size_t size) { (void)size; return false; }
	size_t write(const uint8_t *p, size_t n) { (void)p; (void)n; return 0; }
	bool end(bool even_if_remaining = false) { (void)even_if_remaining; return false; }
	bool hasError(void) { return true; }
	bool isRunning(void) { return false; }
	void abort(void) {}
	void printError(SerialStub &s) { (void)s; }
};
extern UpdateClass Update;
