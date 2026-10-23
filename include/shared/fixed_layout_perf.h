#pragma once
#include <cstddef>
#include <cstdint>
class IStatusReporter;
namespace fixed_perf {
#ifdef DSLIBRIS_DEBUG
uint64_t Now();
void Document(const void *doc, const char *format, const char *file);
void Record(const void *doc, int page, int zoom, const char *stage,
            uint64_t elapsed_us, int ok, size_t bytes = 0, int width = 0,
            int height = 0);
// Captures counters now, using the existing bounded queue. sample must be a
// string literal; snapshots do not measure peaks inside a phase.
void SampleMemory(const void *doc, int page, int zoom, const char *sample);
void BeginView(const void *doc, const char *format, const char *file, int page,
               int zoom, int width, int height);
// Main-thread only; detailed stages stop after the first presentation.
void ViewStage(const char *stage, uint64_t elapsed_us, int ok = 1);
bool NeedsPresentationTiming();
void Drawn(int quality);
void Presented();
void ResetView();
void Flush(IStatusReporter *reporter);
#else
inline uint64_t Now() {
  return 0;
}
inline void Document(const void *, const char *, const char *) {}
inline void Record(const void *, int, int, const char *, uint64_t, int,
                   size_t = 0, int = 0, int = 0) {}
inline void SampleMemory(const void *, int, int, const char *) {}
inline void BeginView(const void *, const char *, const char *, int, int, int,
                      int) {}
inline void ViewStage(const char *, uint64_t, int = 1) {}
inline bool NeedsPresentationTiming() { return false; }
inline void Drawn(int) {}
inline void Presented() {}
inline void ResetView() {}
inline void Flush(IStatusReporter *) {}
#endif
// quality: 1=preview, 2=interactive, 3=final. View methods are main-thread
// only. Record is worker-safe; stages must be string literals. All times are
// microseconds.
class Timer {
  const void *doc_;
  int page_, zoom_;
  const char *stage_;
  uint64_t start_;

public:
  Timer(const void *doc, int page, int zoom, const char *stage)
      : doc_(doc), page_(page), zoom_(zoom), stage_(stage), start_(Now()) {}
  void End(int ok, size_t bytes = 0, int width = 0, int height = 0) {
    Record(doc_, page_, zoom_, stage_, Now() - start_, ok, bytes, width,
           height);
  }
};
} // namespace fixed_perf
