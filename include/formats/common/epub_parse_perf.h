#pragma once

#ifdef DSLIBRIS_DEBUG
#include <3ds.h>

// Main-thread preparation diagnostics. Counters are cumulative; each open
// takes a snapshot, so nested subphase timings must not be added together.
namespace epub_parse_perf {
struct Snapshot {
  u64 chardata_ms, element_ms, flush_ms;
  u32 chardata_calls, element_calls, flush_calls, page_overflows;
  u64 zip_read_ms, transform_ms, xml_ms, css_ms, yield_ms;
  u64 image_probe_ms, zip_index_ms, zip_open_ms;
};
inline Snapshot &Counters() { static Snapshot counters = {}; return counters; }
inline Snapshot Get() { return Counters(); }
class ScopedTime {
  u64 Snapshot::*field_;
  u64 begin_;
public:
  explicit ScopedTime(u64 Snapshot::*field) : field_(field), begin_(osGetTime()) {}
  ~ScopedTime() { Counters().*field_ += osGetTime() - begin_; }
};
class StepGap {
  u64 &last_end_;
public:
  explicit StepGap(u64 &last_end) : last_end_(last_end) {
    if (last_end_) Counters().yield_ms += osGetTime() - last_end_;
  }
  ~StepGap() { last_end_ = osGetTime(); }
};
} // namespace epub_parse_perf
#endif
