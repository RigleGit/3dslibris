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

#include <stdlib.h>
#include "formats/common/reflow_open_profile.h"

namespace reflow_open_stats {
// Largest malloc block, probed without touching memory. Kernel region-free
// cannot be used: libctru commits the application region to the heap at start.
inline bool MallocProbe(unsigned long long bytes, void *) {
  void *p = malloc((size_t)bytes);
  if (!p)
    return false;
  free(p);
  return true;
}
inline unsigned long long HeapHeadroom() {
#ifndef __arm__
  return 0; // host builds never probe huge blocks
#else
  return reflow_open_profile::LargestBlock(64ULL << 20, 1ULL << 20,
                                           MallocProbe, 0);
#endif
}
} // namespace reflow_open_stats
