//	esp_ota_ops スタブ (PC シミュレータ専用)。起動ログと SETUP の情報表示用
#pragma once
#include <stdint.h>

typedef struct {
	const char *label;
	uint32_t address;
} esp_partition_t;

const esp_partition_t *esp_ota_get_running_partition(void);
