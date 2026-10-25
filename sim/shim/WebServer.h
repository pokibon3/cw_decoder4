//
//	WebServer スタブ (PC シミュレータ専用)。ページは配らない
//
#pragma once
#include <Arduino.h>

typedef enum { HTTP_ANY, HTTP_GET, HTTP_POST } HTTPMethod;
typedef enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED } HTTPUploadStatus;

struct HTTPUpload {
	HTTPUploadStatus status = UPLOAD_FILE_START;
	String filename;
	size_t totalSize = 0;
	size_t currentSize = 0;
	uint8_t buf[1] = { 0 };
};

class WebServer {
public:
	typedef void (*Handler)(void);
	WebServer(int port) { (void)port; }
	void collectHeaders(const char **h, size_t n) { (void)h; (void)n; }
	void on(const char *uri, HTTPMethod m, Handler fn) { (void)uri; (void)m; (void)fn; }
	void on(const char *uri, HTTPMethod m, Handler fn, Handler up) { (void)uri; (void)m; (void)fn; (void)up; }
	void onNotFound(Handler fn) { (void)fn; }
	void begin(void) {}
	void stop(void) {}
	void handleClient(void) {}
	void send(int code, const char *type, const String &body) { (void)code; (void)type; (void)body; }
	void sendHeader(const String &name, const String &value, bool first = false) {
		(void)name; (void)value; (void)first;
	}
	String arg(const char *name) { (void)name; return String(""); }
	String header(const char *name) { (void)name; return String(""); }
	String hostHeader(void) { return String(""); }
	HTTPUpload &upload(void) { return up_; }
private:
	HTTPUpload up_;
};
