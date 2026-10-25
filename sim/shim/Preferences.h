//
//	Preferences (NVS) スタブ (PC シミュレータ専用)
//
//	実機の NVS の代わりに 1 つのファイル (既定は sim/nvs.txt) へ保存する。
//	1 行 1 項目「名前空間 キー 16進のバイト列」の形で、消せば工場出荷状態。
//	文字列も UChar もバイト列として持つ
//
#pragma once
#include <Arduino.h>
#include <string>

class Preferences {
public:
	bool begin(const char *ns, bool read_only = false);
	void end(void) {}
	bool isKey(const char *key);
	bool remove(const char *key);
	bool clear(void);

	uint8_t getUChar(const char *key, uint8_t def = 0);
	size_t putUChar(const char *key, uint8_t v);
	String getString(const char *key, const String &def = String());
	size_t putString(const char *key, const char *v);
	size_t getBytes(const char *key, void *buf, size_t len);
	size_t putBytes(const char *key, const void *buf, size_t len);

private:
	std::string ns_;
	bool get(const char *key, std::string *out);
	void put(const char *key, const std::string &v);
};
