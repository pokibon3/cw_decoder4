//	DNSServer スタブ (PC シミュレータ専用)
#pragma once
#include <WiFi.h>

enum class DNSReplyCode { NoError = 0 };

class DNSServer {
public:
	void setErrorReplyCode(DNSReplyCode c) { (void)c; }
	bool start(uint16_t port, const char *domain, const IPAddress &ip) { (void)port; (void)domain; (void)ip; return true; }
	void processNextRequest(void) {}
	void stop(void) {}
};
