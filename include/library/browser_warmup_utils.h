#pragma once

#include <cstdint>

namespace browser_warmup_utils {

static const uint64_t kBrowserWarmupIdleDelayMs = 250;
static const uint64_t kBrowserHeavyWarmupIdleDelayMs = 1200;

inline bool IsBrowserWarmupIdle(uint64_t now_ms, uint64_t last_interaction_ms,
                                bool wait_input_release) {
  if (wait_input_release)
    return false;
  if (now_ms < last_interaction_ms)
    return false;
  return (now_ms - last_interaction_ms) >= kBrowserWarmupIdleDelayMs;
}

inline bool IsBrowserHeavyWarmupIdle(uint64_t now_ms,
                                     uint64_t last_interaction_ms,
                                     bool wait_input_release) {
  if (wait_input_release)
    return false;
  if (now_ms < last_interaction_ms)
    return false;
  return (now_ms - last_interaction_ms) >= kBrowserHeavyWarmupIdleDelayMs;
}

inline bool ShouldQueueCoverWarmup(bool is_selected_book, bool warmup_idle,
                                   bool heavy_warmup_idle) {
  return is_selected_book ? warmup_idle : heavy_warmup_idle;
}

} // namespace browser_warmup_utils
