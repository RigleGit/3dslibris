#pragma once

#include <cstdint>

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;

typedef void *Thread;

typedef struct {
  int stub;
} LightEvent;

typedef struct {
  long x;
  long y;
} FT_Vector;

static inline long long osGetTime(void) { return 0; }
