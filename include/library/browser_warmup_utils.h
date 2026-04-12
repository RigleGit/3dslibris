#pragma once

#include <cstddef>
#include <cstdint>

namespace browser_warmup_utils {

static const uint64_t kBrowserWarmupIdleDelayMs = 250;
static const uint64_t kBrowserHeavyWarmupIdleDelayMs = 1200;
static const uint64_t kOld3dsSelectedCoverMinFreeBytes = 22u * 1024u * 1024u;
static const uint64_t kOld3dsWarmCoverMinFreeBytes = 26u * 1024u * 1024u;
static const uint64_t kOld3dsSelectedCoverRetryDelayMs = 2000;
static const uint64_t kOld3dsWarmCoverRetryDelayMs = 8000;
static const size_t kOld3dsWarmupQueueLimit = 1;

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

inline bool ShouldQueueCoverWarmupForDevice(bool is_new_3ds,
                                            bool is_selected_book,
                                            bool warmup_idle,
                                            bool heavy_warmup_idle,
                                            size_t queued_heavy_jobs) {
  if (is_new_3ds)
    return ShouldQueueCoverWarmup(is_selected_book, warmup_idle,
                                  heavy_warmup_idle);
  if (queued_heavy_jobs >= kOld3dsWarmupQueueLimit)
    return false;
  if (is_selected_book)
    return warmup_idle;
  return heavy_warmup_idle;
}

inline bool HasCoverExtractionHeadroom(bool is_new_3ds, bool is_selected_book,
                                       uint64_t free_bytes) {
  if (is_new_3ds)
    return true;
  const uint64_t min_bytes = is_selected_book ? kOld3dsSelectedCoverMinFreeBytes
                                              : kOld3dsWarmCoverMinFreeBytes;
  return free_bytes >= min_bytes;
}

inline uint64_t CoverRetryDelayMs(bool is_new_3ds, bool is_selected_book,
                                  int rc, bool has_pixels) {
  if (is_new_3ds)
    return 0;
  if (rc == 4 || !has_pixels) {
    return is_selected_book ? kOld3dsSelectedCoverRetryDelayMs
                            : kOld3dsWarmCoverRetryDelayMs;
  }
  return 0;
}

} // namespace browser_warmup_utils
