#include "shared/fixed_layout_perf.h"
#ifdef DSLIBRIS_DEBUG
#include "shared/status_reporter.h"
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace fixed_perf {
namespace {
// Fixed storage: no allocations or file/UI access from render workers.
struct Event {
  const void *doc;
  const char *stage;
  const char *sample;
  uint64_t at, duration;
  size_t bytes;
  int page, zoom, width, height, ok;
  unsigned view, heap_used, heap_free, linear_free, region_free;
  char file[128];
};
const unsigned kCapacity = 64;
Event queue[kCapacity];
unsigned read_index = 0, count = 0, dropped = 0;
struct QueueLock {
  LightLock value;
  QueueLock() {
    LightLock_Init(&value);
  }
} queue_lock;
void Lock() {
  LightLock_Lock(&queue_lock.value);
}
void Unlock() {
  LightLock_Unlock(&queue_lock.value);
}
void Push(const Event &event) {
  Lock();
  if (count == kCapacity)
    ++dropped;
  else {
    queue[(read_index + count) % kCapacity] = event;
    ++count;
  }
  Unlock();
}
struct View {
  const void *doc = nullptr;
  int page = -1, zoom = -1, width = 0, height = 0, drawn = 0;
  unsigned seen = 0, id = 0;
  uint64_t start = 0, drawn_at = 0;
} view;
unsigned next_view = 0;
} // namespace
uint64_t Now() {
  const uint64_t ticks = svcGetSystemTick();
  return (ticks / SYSCLOCK_ARM11) * 1000000ULL +
         (ticks % SYSCLOCK_ARM11) * 1000000ULL / SYSCLOCK_ARM11;
}
void Record(const void *doc, int page, int zoom, const char *stage,
            uint64_t elapsed_us, int ok, size_t bytes, int width, int height) {
  Event e = {};
  e.doc = doc;
  e.page = page;
  e.zoom = zoom;
  e.stage = stage;
  e.at = Now();
  e.duration = elapsed_us;
  e.ok = ok;
  e.bytes = bytes;
  e.width = width;
  e.height = height;
  Push(e);
}
void Document(const void *doc, const char *format, const char *file) {
  Event e = {};
  e.doc = doc;
  e.page = -1;
  e.zoom = -1;
  e.stage = format;
  e.at = Now();
  std::snprintf(e.file, sizeof(e.file), "%s", file ? file : "");
  Push(e);
}
void BeginView(const void *doc, const char *format, const char *file, int page,
               int zoom, int width, int height) {
  if (view.doc == doc && view.page == page && view.zoom == zoom &&
      view.width == width && view.height == height)
    return;
  view = View();
  view.doc = doc;
  view.page = page;
  view.zoom = zoom;
  view.width = width;
  view.height = height;
  view.id = ++next_view;
  view.start = Now();
  Event e = {};
  e.doc = doc;
  e.page = page;
  e.zoom = zoom;
  e.stage = format;
  e.at = view.start;
  e.view = view.id;
  e.width = width;
  e.height = height;
  std::snprintf(e.file, sizeof(e.file), "%s", file ? file : "");
  Push(e);
}
void ViewStage(const char *stage, uint64_t elapsed_us, int ok) {
  if (view.doc && !view.seen)
    Record(view.doc, view.page, view.zoom, stage, elapsed_us, ok);
}
bool NeedsPresentationTiming() {
  return view.doc && view.drawn && !view.seen;
}
void Drawn(int quality) {
  view.drawn = quality;
  if (!view.seen)
    view.drawn_at = Now();
}
void ResetView() {
  view = View();
}
void Presented() {
  if (!view.doc || !view.drawn)
    return;
  const int quality = view.drawn;
  view.drawn = 0;
  const unsigned bit = 1u << quality;
  if (view.seen & bit)
    return;
  if (!view.seen) {
    Record(view.doc, view.page, view.zoom, "first_present", Now() - view.start,
           1);
    Record(view.doc, view.page, view.zoom, "draw_to_present",
           Now() - view.drawn_at, 1);
  }
  view.seen |= bit;
  Record(view.doc, view.page, view.zoom,
         quality == 3   ? "final_present"
         : quality == 2 ? "interactive_present"
                        : "preview_present",
         Now() - view.start, 1);
  SampleMemory(view.doc, view.page, view.zoom, "present");
}
void SampleMemory(const void *doc, int page, int zoom, const char *sample) {
  Event e = {};
  e.doc = doc;
  e.page = page;
  e.zoom = zoom;
  e.stage = "memory";
  e.sample = sample;
  e.at = Now();
  const struct mallinfo mi = mallinfo();
  e.heap_used = (unsigned)mi.uordblks;
  e.heap_free = (unsigned)mi.fordblks;
  e.linear_free = linearSpaceFree();
  e.region_free = osGetMemRegionFree(MEMREGION_ALL);
  Push(e);
}
void Flush(IStatusReporter *reporter) {
  if (!reporter)
    return;
  // Bound work per frame even if a worker keeps producing events.
  for (unsigned i = 0; i < kCapacity; ++i) {
    Event e = {};
    Lock();
    if (!count) {
      Unlock();
      break;
    }
    e = queue[read_index];
    read_index = (read_index + 1) % kCapacity;
    --count;
    Unlock();
    char line[384];
    if (e.view || e.file[0])
      std::snprintf(line, sizeof(line),
                    "PERF view=%u doc=%p format=%s page=%d zoom=%d "
                    "target=%dx%d at_us=%llu file=\"%s\"",
                    e.view, e.doc, e.stage, e.page + 1, e.zoom, e.width,
                    e.height, (unsigned long long)e.at, e.file);
    else if (std::strcmp(e.stage, "memory") == 0)
      std::snprintf(line, sizeof(line),
                    "PERF doc=%p page=%d zoom=%d stage=memory at_us=%llu "
                    "heap_used=%u heap_free=%u linear_free=%u region_free=%u "
                    "sample=%s",
                    e.doc, e.page + 1, e.zoom, (unsigned long long)e.at,
                    e.heap_used, e.heap_free, e.linear_free, e.region_free,
                    e.sample ? e.sample : "unknown");
    else
      std::snprintf(line, sizeof(line),
                    "PERF doc=%p page=%d zoom=%d stage=%s us=%llu at_us=%llu "
                    "ok=%d bytes=%lu size=%dx%d",
                    e.doc, e.page + 1, e.zoom, e.stage,
                    (unsigned long long)e.duration, (unsigned long long)e.at,
                    e.ok, (unsigned long)e.bytes, e.width, e.height);
    reporter->PrintStatus(line);
  }
  Lock();
  unsigned lost = dropped;
  dropped = 0;
  Unlock();
  if (lost) {
    char line[64];
    std::snprintf(line, sizeof(line), "PERF dropped=%u", lost);
    reporter->PrintStatus(line);
  }
}
} // namespace fixed_perf
#endif
