//
//	ADC 生サンプルの取り込み (仕様は rawcap.h)
//
#include "rawcap.h"
#if RAWCAP
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsp.h"

#define RAWCAP_RATE (DSP_SAMPLE_RATE * 4)
#define RAWCAP_PER_LINE 64

volatile uint8_t rawcap_state = 0;
static uint16_t *buf = nullptr;
static uint32_t cap_n = 0;             // 確保できた個数 (RAWCAP_SAMPLES 以下)
static volatile uint32_t count = 0;     // 一括: 取り込んだ個数 / 連続: 書き込んだ総数
static uint32_t sent = 0;               // 送出済みの個数 (連続では読み出した総数)
static uint32_t dropped = 0;            // 連続: 送り切れずに捨てた個数
static uint8_t header_sent = 0;
static uint32_t t_start = 0;

void rawcap_store(uint16_t v)
{
	uint32_t n = count;
	buf[n++] = v;
	count = n;
	if (n >= cap_n) rawcap_state = 2;
}

void rawcap_stream(uint16_t v)
{
	uint32_t n = count;
	buf[n % cap_n] = v;
	count = n + 1;
}

//	TX バッファに入るときだけ書く (scopelog と同じ。ブロックしない)
static bool put_line(const char *line)
{
	size_t len = strlen(line);
	if ((size_t)Serial.availableForWrite() < len) return false;
	Serial.write((const uint8_t *)line, len);
	return true;
}

//	ヒープが断片化していて 32KB が連続で取れないことがあるので、
//	取れるまで半分ずつ減らす (4096 個まで)
static bool alloc_buf(void)
{
	for (uint32_t n = RAWCAP_SAMPLES; !buf && n >= 4096; n /= 2) {
		buf = (uint16_t *)malloc(n * sizeof(uint16_t));
		if (buf) cap_n = n;
	}
	if (buf) return true;
	char line[64];
	snprintf(line, sizeof(line), "R! alloc failed (max block %u bytes)\n",
	         (unsigned)ESP.getMaxAllocHeap());
	Serial.print(line);
	return false;
}

//	p に 3 桁 16 進で n 個書き、改行で閉じる
static void put_hex(char *p, const uint16_t *src, uint32_t n)
{
	static const char hex[] = "0123456789abcdef";
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = src[i];
		*p++ = hex[(v >> 8) & 0xF];
		*p++ = hex[(v >> 4) & 0xF];
		*p++ = hex[v & 0xF];
	}
	*p++ = '\n';
	*p = 0;
}

static void poll_stream(void)
{
	char line[RAWCAP_PER_LINE * 3 + 16];
	for (;;) {
		uint32_t w = count;
		// DSP タスクが書き進めている最中の 1 行分は読まない
		if (w - sent > cap_n - RAWCAP_PER_LINE) {
			uint32_t skip = (w - sent) - (cap_n - RAWCAP_PER_LINE);
			sent += skip;
			dropped += skip;
		}
		if (w - sent < RAWCAP_PER_LINE) return;
		uint16_t tmp[RAWCAP_PER_LINE];
		for (uint32_t i = 0; i < RAWCAP_PER_LINE; i++) tmp[i] = buf[(sent + i) % cap_n];
		int k = snprintf(line, sizeof(line), "Q %lu ", (unsigned long)sent);
		put_hex(line + k, tmp, RAWCAP_PER_LINE);
		if (!put_line(line)) return;    // 次フレームに続きを送る
		sent += RAWCAP_PER_LINE;
	}
}

void rawcap_poll(void)
{
	while (Serial.available() > 0) {
		int c = Serial.read();
		char line[64];
		if (c == 'r' && rawcap_state == 0) {
			if (!alloc_buf()) continue;
			count = 0;
			sent = 0;
			header_sent = 0;
			t_start = millis();
			rawcap_state = 1;
		} else if (c == 's' && rawcap_state == 0) {
			if (!alloc_buf()) continue;
			count = 0;
			sent = 0;
			dropped = 0;
			snprintf(line, sizeof(line), "Q0 rate=%u ring=%u\n",
			         (unsigned)DSP_SAMPLE_RATE, (unsigned)cap_n);
			Serial.print(line);
			rawcap_state = 3;
		} else if (c == 'x' && rawcap_state == 3) {
			rawcap_state = 0;
			poll_stream();
			snprintf(line, sizeof(line), "Q1 total=%lu dropped=%lu\n",
			         (unsigned long)sent, (unsigned long)dropped);
			Serial.print(line);
		}
	}

	if (rawcap_state == 3) {
		poll_stream();
		return;
	}
	if (rawcap_state != 2) return;
	char line[RAWCAP_PER_LINE * 3 + 4];
	if (!header_sent) {
		snprintf(line, sizeof(line), "R0 n=%u rate=%u ms=%lu\n",
		         (unsigned)cap_n, (unsigned)RAWCAP_RATE, (unsigned long)t_start);
		if (!put_line(line)) return;
		header_sent = 1;
	}
	while (sent < cap_n) {
		uint32_t n = cap_n - sent;
		if (n > RAWCAP_PER_LINE) n = RAWCAP_PER_LINE;
		line[0] = 'R';
		line[1] = ' ';
		put_hex(line + 2, buf + sent, n);
		if (!put_line(line)) return;    // 次フレームに続きを送る
		sent += n;
	}
	if (!put_line("R1\n")) return;
	rawcap_state = 0;
}
#endif
