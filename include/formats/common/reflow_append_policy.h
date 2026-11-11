#pragma once
// When the cooperative open hands closed pages to the cache writer.
//
// Every document the writer receives ends its text and search blocks, so
// handing it a few pages at a time yields tiny blocks instead of the 64 KiB
// blocks the format is sized for. Pages are batched until their text is worth
// several full blocks, the parse finished, or memory ran low.
#include "formats/common/page_cache_codec.h"

namespace reflow_append_policy {
static const unsigned kBlockWords = (unsigned)page_cache_codec::kBlockWords;
// Default batch: four full text blocks per document.
static const unsigned kDefaultFlushWords = 4 * kBlockWords;
// Bound on pages held back regardless of their size (empty pages).
static const unsigned kMaxPendingPages = 4096;

inline bool ShouldAppend(unsigned pending_pages, unsigned pending_words,
                         bool parse_finished, bool low_memory,
                         unsigned flush_words = kDefaultFlushWords) {
  if (!pending_pages)
    return false;
  return parse_finished || low_memory || pending_words >= flush_words ||
         pending_pages >= kMaxPendingPages;
}
} // namespace reflow_append_policy
