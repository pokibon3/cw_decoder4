//	mDNS スタブ (PC シミュレータ専用)
#pragma once
#include <Arduino.h>

class MDNSResponder {
public:
	bool begin(const char *host) { (void)host; return true; }
	void addService(const char *svc, const char *proto, uint16_t port) { (void)svc; (void)proto; (void)port; }
	void end(void) {}
};
extern MDNSResponder MDNS;
