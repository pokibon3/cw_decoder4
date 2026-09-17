//
//	スコープログ本体 (フォーマットは scopelog.h)
//
#include <Arduino.h>
#include <stdio.h>
#include "scopelog.h"
#include "dsp.h"
#include "decoder.h"
#include "audio.h"
#include "version.h"

#define LOG_CHAR_N 32
#define HEADER_INTERVAL_MS 5000
#define LEVEL_INTERVAL_MS 1000

typedef struct { uint8_t ch; uint32_t col; } log_char_t;

static volatile uint8_t enabled = 0;
static uint32_t next_col = 0;           // 次に送る列番号
static uint32_t dropped = 0;
static uint32_t t_header = 0;
static uint32_t t_level = 0;
static uint16_t s_wpm = 0xFFFF, s_tone = 0xFFFF, s_hopq8 = 0;
static uint8_t s_auto = 0xFF;

static log_char_t chars[LOG_CHAR_N];
static volatile uint8_t ch_head = 0;    // DSP タスクが進める
static uint8_t ch_tail = 0;             // 表示ループが進める

//	TX バッファに入るときだけ書く。入らなければ false (呼び出し側で欠落扱い)
static bool put_line(const char *line)
{
	size_t len = strlen(line);
	if ((size_t)Serial.availableForWrite() < len) {
		return false;
	}
	Serial.write((const uint8_t *)line, len);
	return true;
}

void scopelog_set_enabled(uint8_t on)
{
	if (on && !enabled) {
		next_col = dsp_scope_col_index();
		dropped = 0;
		t_header = 0;                   // 直後にヘッダを出す
		t_level = 0;
		s_wpm = 0xFFFF; s_tone = 0xFFFF; s_hopq8 = 0; s_auto = 0xFF;
		ch_tail = ch_head;
	}
	enabled = on ? 1 : 0;
}

uint8_t scopelog_enabled(void)
{
	return enabled;
}

void scopelog_char(uint8_t ch, uint32_t col)
{
	if (!enabled) return;
	uint8_t h = ch_head;
	chars[h].ch = ch;
	chars[h].col = col;
	ch_head = (uint8_t)((h + 1) % LOG_CHAR_N);
}

//	デコーダの出力バイト → UTF-8 (display.cpp と同じ対応。ホレ/ラタは「」)
static void char_utf8(uint8_t ch, char *out)
{
	static const uint16_t kana_cp[0x3F] = {
		0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2,
		0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
		0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,
		0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA,
		0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,
		0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,
		0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,
		0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE,
		0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,
		0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2,
		0x30E4, 0x30E6, 0x30E8,
		0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED,
		0x30EF, 0x30F3, 0x309B, 0x309C
	};
	uint16_t cp;
	if (ch == 5) cp = 0x300C;
	else if (ch == 6) cp = 0x300D;
	else if (ch >= 0xA1 && ch <= 0xDF) cp = kana_cp[ch - 0xA1];
	else if (ch < 0x80) cp = ch;
	else cp = '*';
	if (cp < 0x80) {
		out[0] = (char)cp; out[1] = 0;
	} else {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		out[3] = 0;
	}
}

void scopelog_poll(void)
{
	if (!enabled) return;
	char line[96];
	uint32_t now = millis();

	if (t_header == 0 || (now - t_header) >= HEADER_INTERVAL_MS) {
		snprintf(line, sizeof(line), "H rate=%d hop=%d cols=150 ver=%s\n",
		         DSP_SAMPLE_RATE, DSP_HOP, FW_VERSION);
		if (put_line(line)) t_header = now;
	}

	// 状態行 (変化時のみ)
	{
		uint16_t wpm = decoder_wpm();
		uint16_t tone = dsp_tone_hz();
		uint8_t au = dsp_tone_is_auto();
		uint16_t hopq8 = dsp_scope_period_q8();
		if (wpm != s_wpm || tone != s_tone || au != s_auto || hopq8 != s_hopq8) {
			snprintf(line, sizeof(line), "T %lu wpm=%u tone=%u auto=%u hopq8=%u\n",
			         (unsigned long)dsp_scope_col_index(), wpm, tone, au, hopq8);
			if (put_line(line)) {
				s_wpm = wpm; s_tone = tone; s_auto = au; s_hopq8 = hopq8;
			}
		}
	}

	// スコープ列 (前回送った続きから)
	{
		scope_col_t cols[16];
		uint32_t first = 0;
		uint32_t lost = 0;
		int n = dsp_get_scope_since(next_col, cols, 16, &first, &lost);
		dropped += lost;
		for (int i = 0; i < n; i++) {
			uint32_t idx = first + (uint32_t)i;
			snprintf(line, sizeof(line), "S %lu %lu %d %d %u %u\n",
			         (unsigned long)idx, (unsigned long)cols[i].t_ms,
			         cols[i].mn, cols[i].mx, cols[i].mag, cols[i].gate);
			if (!put_line(line)) {
				dropped++;              // 入らない列は捨てて先へ進む
			}
			next_col = idx + 1;
		}
	}

	// デコード文字
	while (ch_tail != ch_head) {
		char u8[4];
		char_utf8(chars[ch_tail].ch, u8);
		snprintf(line, sizeof(line), "C %lu %s\n", (unsigned long)chars[ch_tail].col, u8);
		if (!put_line(line)) break;     // 次フレームに回す
		ch_tail = (uint8_t)((ch_tail + 1) % LOG_CHAR_N);
	}

	// 入力レベル (毎秒)
	if ((now - t_level) >= LEVEL_INTERVAL_MS) {
		snprintf(line, sizeof(line), "L %d %lu\n", (int)dsp_input_peak(),
		         (unsigned long)audio_clip_total());
		if (put_line(line)) t_level = now;
	}

	if (dropped) {
		snprintf(line, sizeof(line), "X dropped=%lu\n", (unsigned long)dropped);
		if (put_line(line)) dropped = 0;
	}
}
