//
//	フリースタンディング WASM ビルドに必要な libc / libm の最小実装。
//
//	wasm32 を -nostdlib でビルドするので libc は無い。中核コードが実際に
//	使うものだけをここで供給する (memcpy/memset 系は clang が構造体コピー
//	などで暗黙に呼ぶので、使われていなくても必要)。
//
//	三角関数は JS の Math.cos / Math.sin を import する。倍精度で計算して
//	から float に落とすので、libm の float 版より誤差が小さい。呼ばれるのは
//	Goertzel 係数の更新 (トーン切替時) と Hann 窓の生成 (起動時) だけで、
//	ブロック処理のホットパスには入らない。
//
#include <stddef.h>
#include <stdint.h>

//==================================================================
//	数学関数
//==================================================================
__attribute__((import_module("env"), import_name("cos"))) double js_cos(double x);
__attribute__((import_module("env"), import_name("sin"))) double js_sin(double x);

float cosf(float x) { return (float)js_cos((double)x); }
float sinf(float x) { return (float)js_sin((double)x); }

//	wasm の f32.sqrt / f64.sqrt 命令に落ちる (-fno-math-errno が必要)
float sqrtf(float x) { return __builtin_sqrtf(x); }
double sqrt(double x) { return __builtin_sqrt(x); }

//==================================================================
//	文字列 / メモリ
//==================================================================
void *memset(void *d, int c, size_t n)
{
	unsigned char *p = (unsigned char *)d;
	while (n--) *p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = (unsigned char *)d;
	const unsigned char *q = (const unsigned char *)s;
	while (n--) *p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = (unsigned char *)d;
	const unsigned char *q = (const unsigned char *)s;
	if (p == q || n == 0) return d;
	if (p < q) {
		while (n--) *p++ = *q++;
	} else {
		p += n;
		q += n;
		while (n--) *--p = *--q;
	}
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = (const unsigned char *)a;
	const unsigned char *q = (const unsigned char *)b;
	while (n--) {
		if (*p != *q) return (int)*p - (int)*q;
		p++; q++;
	}
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
	char *p = d;
	while ((*p++ = *s++) != '\0') ;
	return d;
}

char *strcat(char *d, const char *s)
{
	char *p = d;
	while (*p) p++;
	while ((*p++ = *s++) != '\0') ;
	return d;
}
