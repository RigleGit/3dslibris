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

typedef struct {
  u16 px;
  u16 py;
} touchPosition;

#ifdef DSLIBRIS_TEST_CLOCK
extern long long dslibris_test_clock;
extern long long dslibris_test_clock_step;
static inline long long osGetTime(void) {
  dslibris_test_clock += dslibris_test_clock_step;
  return dslibris_test_clock;
}
#else
static inline long long osGetTime(void) { return 0; }
#endif
static inline void aptSetHomeAllowed(bool) {}
