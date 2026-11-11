#pragma once
// Clock and memory probes for open-path accounting and tier selection. Non-ARM hosts have no system tick,
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

namespace reflow_open_stats {
// Free bytes in the application memory region (0 off-device).
inline unsigned long long FreeApplicationMemory() {
#ifndef __arm__
  return 0;
#else
  return (unsigned long long)osGetMemRegionFree(MEMREGION_APPLICATION);
#endif
}
} // namespace reflow_open_stats
