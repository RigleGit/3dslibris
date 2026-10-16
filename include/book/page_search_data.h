#pragma once

#include <algorithm>
#include <stdint.h>
#include <vector>

// Sparse annotations at token offsets; never part of the rendering buffer.
namespace page_search {
enum Flags { Skip = 1, Space = 2, Barrier = 4, RtlStart = 8, RtlEnd = 16 };
typedef std::vector<uint32_t> Events;
inline unsigned FlagsAt(const Events &events, uint32_t offset) {
  Events::const_iterator it = std::lower_bound(events.begin(), events.end(), offset << 5);
  return it != events.end() && (*it >> 5) == offset ? (*it & 31) : 0;
}
inline void Annotate(Events &events, uint32_t offset, unsigned flags) {
  // Emission may have removed a trailing space since the previous annotation.
  bool trimmed_rtl_end = false;
  while (!events.empty() && (events.back() >> 5) > offset) {
    if (events.back() & RtlEnd) trimmed_rtl_end = true;
    if (events.back() & RtlStart) trimmed_rtl_end = false;
    events.pop_back();
  }
  if (trimmed_rtl_end) {
    if (!events.empty() && (events.back() >> 5) == offset &&
        (events.back() & RtlStart)) {
      // The removed end belonged to a run starting at this very offset.
      // Remove only that start; an adjacent run's existing end must survive.
      events.back() &= ~RtlStart;
      if (!(events.back() & 31)) events.pop_back();
    } else flags |= RtlEnd;
  }
  if (!events.empty() && (events.back() >> 5) == offset)
    events.back() |= flags;
  else
    events.push_back((offset << 5) | flags);
}
inline bool Valid(const Events &events, uint32_t length) {
  if (events.size() > length + 1) return false;
  bool rtl = false;
  for (size_t i = 0; i < events.size(); ++i) {
    if (!(events[i] & 31) || (events[i] >> 5) > length ||
        (i && (events[i-1] >> 5) >= (events[i] >> 5))) return false;
    if (events[i] & RtlEnd) { if (!rtl) return false; rtl = false; }
    if (events[i] & RtlStart) { if (rtl) return false; rtl = true; }
  }
  return !rtl;
}
} // namespace page_search
