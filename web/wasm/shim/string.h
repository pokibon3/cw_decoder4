//
//	string.h スタブ (Web (WASM) ビルド専用、フリースタンディング)
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcat(char *d, const char *s);
char *strcpy(char *d, const char *s);

#ifdef __cplusplus
}
#endif
