#pragma once
#include <stdint.h>
struct Preferences {
  bool begin(const char *, bool) { return true; }
  uint32_t getUInt(const char *, uint32_t d) { return d; }
  void putUInt(const char *, uint32_t) {}
};
