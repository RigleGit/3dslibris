#pragma once

#include <algorithm>

namespace browser_cover_cache_utils {

struct VisibleRange {
  int start;
  int end;
};

inline VisibleRange ComputeVisibleRange(int page_start, int total_books,
                                        int page_size) {
  VisibleRange range = {0, 0};
  if (total_books <= 0 || page_size <= 0)
    return range;

  range.start = std::max(0, page_start);
  range.end = std::min(total_books, range.start + page_size);
  if (range.end < range.start)
    range.end = range.start;
  return range;
}

inline bool RangeContains(const VisibleRange &range, int index) {
  return index >= range.start && index < range.end;
}

} // namespace browser_cover_cache_utils
