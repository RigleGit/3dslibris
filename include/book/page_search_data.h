#pragma once

#include <algorithm>
#include <stdint.h>
#include <vector>

// Sparse annotations at token offsets; never part of the rendering buffer.
namespace page_search {
enum Flags { Skip = 1, Space = 2, Barrier = 4 };
typedef std::vector<uint32_t> Events;
inline unsigned FlagsAt(const Events &events, uint32_t offset) {
  Events::const_iterator it = std::lower_bound(events.begin(), events.end(), offset << 3);
  return it != events.end() && (*it >> 3) == offset ? (*it & 7) : 0;
}
inline void Annotate(Events &events, uint32_t offset, unsigned flags) {
  // Emission may have removed a trailing space since the previous annotation.
  while (!events.empty() && (events.back() >> 3) > offset) events.pop_back();
  if (!events.empty() && (events.back() >> 3) == offset)
    events.back() |= flags;
  else
    events.push_back((offset << 3) | flags);
}
inline bool Valid(const Events &events, uint32_t length) {
  if (events.size() > length + 1) return false;
  for (size_t i = 0; i < events.size(); ++i)
    if (!(events[i] & 7) || (events[i] >> 3) > length ||
        (i && (events[i-1] >> 3) >= (events[i] >> 3))) return false;
  return true;
}
} // namespace page_search
