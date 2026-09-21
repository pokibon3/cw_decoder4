//
//	math.h スタブ (Web (WASM) ビルド専用、フリースタンディング)
//	中核コードが使う数学関数は cosf / sinf / sqrtf / sqrt の 4 つだけ。
//
#pragma once

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C" {
#endif

float cosf(float x);
float sinf(float x);
float sqrtf(float x);
double sqrt(double x);

#ifdef __cplusplus
}
#endif
