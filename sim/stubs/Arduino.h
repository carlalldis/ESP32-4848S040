#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <random>
inline uint32_t g_stub_millis = 0;
inline uint32_t millis() { return g_stub_millis; }
inline uint32_t esp_random() { static std::mt19937 r(1); return r(); }
template <class T> inline T min(T a, T b) { return a < b ? a : b; }
template <class T> inline T max(T a, T b) { return a > b ? a : b; }
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define IRAM_ATTR
