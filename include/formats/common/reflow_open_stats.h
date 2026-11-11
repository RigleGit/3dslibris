#pragma once
// Per-phase accumulators for the cooperative MOBI/FB2 open. Pure data: callers
// supply microsecond durations, so the helper has no platform dependency and is
// host-testable. Slots overlap on purpose (Parse contains the parser phases and
// Append contains the writer work), so slot times must not be summed.
#include <cstddef>
#include <cstdio>
#include <stdint.h>

namespace reflow_open_stats {
enum Slot {
  Read,
  Header,
  Merge,
  Decode,
  Markup,
  Cleanup,
  TocPrepare,
  Init,
  Pages,
  Toc,
  Xml,
  Fallback,
  Parse,
  Append,
  Commit,
  Validate,
  SlotCount
};
inline const char *SlotName(unsigned slot) {
  static const char *const names[SlotCount] = {
      "read",   "header", "merge", "decode",  "markup", "cleanup", "tocprep",
      "init",   "pages",  "toc",   "xml",     "fallback", "parse", "append",
      "commit", "validate"};
  return slot < SlotCount ? names[slot] : "?";
}
struct Stats {
  uint64_t us[SlotCount];
  uint32_t calls[SlotCount];
  uint32_t max_us[SlotCount];
  // Pages produced by Pages-slot calls.
  uint32_t page_calls, pages_total, pages_max;
  Stats() { Reset(); }
  void Reset() {
    for (unsigned i = 0; i < SlotCount; ++i)
      us[i] = calls[i] = max_us[i] = 0;
    page_calls = pages_total = pages_max = 0;
  }
  void Add(Slot slot, uint64_t micros) {
    if (slot >= SlotCount)
      return;
    us[slot] += micros;
    ++calls[slot];
    const uint32_t m = micros > 0xffffffffULL ? 0xffffffffU : (uint32_t)micros;
    if (m > max_us[slot])
      max_us[slot] = m;
  }
  void NotePages(unsigned produced) {
    ++page_calls;
    pages_total += produced;
    if (produced > pages_max)
      pages_max = produced;
  }
  uint64_t Ms(Slot slot) const { return slot < SlotCount ? us[slot] / 1000 : 0; }
  // "read=12/3 merge=40/700 ..." (ms/calls), idle slots omitted.
  size_t FormatPhases(char *out, size_t size) const {
    if (!out || !size)
      return 0;
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < SlotCount; ++i) {
      if (!calls[i])
        continue;
      const int w = snprintf(out + n, size - n, "%s%s=%llu/%u", n ? " " : "",
                             SlotName(i), (unsigned long long)(us[i] / 1000),
                             (unsigned)calls[i]);
      if (w < 0 || (size_t)w >= size - n)
        break;
      n += (size_t)w;
    }
    return n;
  }
};
} // namespace reflow_open_stats
