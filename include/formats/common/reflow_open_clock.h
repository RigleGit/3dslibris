#pragma once
// Microsecond clock for open-path accounting. Non-ARM hosts have no system tick,
// so they fall back to the stubbed millisecond clock.
#include <3ds.h>
#include <stdint.h>

namespace reflow_open_stats {
inline uint64_t NowUs() {
#ifndef __arm__
  return (uint64_t)osGetTime() * 1000ULL;
#else
  const uint64_t ticks = svcGetSystemTick();
  return (ticks / SYSCLOCK_ARM11) * 1000000ULL +
         (ticks % SYSCLOCK_ARM11) * 1000000ULL / SYSCLOCK_ARM11;
#endif
}
} // namespace reflow_open_stats
