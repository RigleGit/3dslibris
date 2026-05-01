#pragma once

#include <cstddef>
#include <cstdint>

namespace browser_warmup_utils {

static const uint64_t kBrowserWarmupIdleDelayMs = 250;
static const uint64_t kBrowserHeavyWarmupIdleDelayMs = 1200;
static const uint64_t kNew3dsHeavyWarmupIdleDelayMs = 600;
static const uint64_t kBrowserInputReleaseMaxWaitMs = 1500;
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

inline bool IsBrowserHeavyWarmupIdleForDevice(bool is_new_3ds,
                                              uint64_t now_ms,
                                              uint64_t last_interaction_ms,
                                              bool wait_input_release) {
  if (wait_input_release)
    return false;
  if (now_ms < last_interaction_ms)
    return false;
  const uint64_t delay_ms =
      is_new_3ds ? kNew3dsHeavyWarmupIdleDelayMs
                 : kBrowserHeavyWarmupIdleDelayMs;
  return (now_ms - last_interaction_ms) >= delay_ms;
}

inline bool ShouldForceClearInputRelease(uint64_t now_ms,
                                         uint64_t last_interaction_ms,
                                         bool wait_input_release) {
  if (!wait_input_release)
    return false;
  if (now_ms < last_interaction_ms)
    return false;
  return (now_ms - last_interaction_ms) >= kBrowserInputReleaseMaxWaitMs;
}

inline int VisibleBrowserEntryCount(int page_size, int page_start,
                                    int book_count) {
  if (page_size <= 0 || page_start < 0 || book_count <= page_start)
    return 0;
  const int remaining = book_count - page_start;
  return remaining < page_size ? remaining : page_size;
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

inline bool ShouldAttemptPdfCoverWarmup(bool /*is_new_3ds*/) {
  // Safe on both old and new 3DS: pdf_extract_cover now calls
  // EstimateMuPdfPageRenderComplexity before rendering and returns rc=8 for
  // pages that would OOM, so the cover extraction path no longer crashes.
  return true;
}

// Returns true for error codes that represent a permanent cover extraction
// failure — i.e. retrying will never succeed regardless of available memory.
// rc=3: password-protected document; rc=8: page too complex to render.
inline bool IsPermanentCoverFailure(int rc) {
  return rc == 3 || rc == 8;
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

inline bool MetadataWarmupDone(bool supports_metadata_indexing,
                               bool metadata_index_tried) {
  return !supports_metadata_indexing || metadata_index_tried;
}

inline bool CoverWarmupDone(bool supports_cover_warmup, bool has_cover_pixels,
                            unsigned int cover_attempts,
                            unsigned int cover_attempt_limit) {
  return !supports_cover_warmup || has_cover_pixels ||
         cover_attempts >= cover_attempt_limit;
}

inline bool BookWarmupDone(bool supports_metadata_indexing,
                           bool metadata_index_tried,
                           bool supports_cover_warmup,
                           bool has_cover_pixels,
                           unsigned int cover_attempts,
                           unsigned int cover_attempt_limit) {
  return MetadataWarmupDone(supports_metadata_indexing, metadata_index_tried) &&
         CoverWarmupDone(supports_cover_warmup, has_cover_pixels,
                         cover_attempts, cover_attempt_limit);
}

} // namespace browser_warmup_utils
