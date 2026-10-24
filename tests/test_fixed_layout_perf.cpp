#include "shared/fixed_layout_perf.h"
#include "shared/status_reporter.h"
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <cstdio>
uint64_t fake_tick = 1000;
unsigned fake_region_free = 1234;
unsigned fake_linear_free = 5678;
int fake_heap_used = 345;
int fake_heap_free = 678;
struct Reporter : IStatusReporter {
  std::vector<std::string> lines;
  void PrintStatus(const char *s) {
    lines.push_back(s);
  }
  void PrintStatus(std::string s) {
    lines.push_back(s);
  }
};
int main() {
  Reporter r;
  int doc;
  fixed_perf::SampleMemory(&doc, -1, -1, "mobi.after_read");
  assert(r.lines.empty());
  fake_tick = 2000;
  fake_heap_used = 901;
  fake_heap_free = 234;
  fake_region_free = 4567;
  fake_linear_free = 8901;
  fixed_perf::SampleMemory(&doc, -1, -1, "mobi.after_decode");
  fixed_perf::Flush(&r);
  assert(r.lines.size() == 2);
  assert(r.lines[0].find("stage=memory at_us=1000 ") != std::string::npos);
  assert(r.lines[0].find("heap_used=345 heap_free=678 linear_free=5678 region_free=1234") != std::string::npos);
  assert(r.lines[0].find("sample=mobi.after_read") != std::string::npos);
  assert(r.lines[1].find("stage=memory at_us=2000 ") != std::string::npos);
  assert(r.lines[1].find("heap_used=901 heap_free=234 linear_free=8901 region_free=4567") != std::string::npos);
  assert(r.lines[1].find("sample=mobi.after_decode") != std::string::npos);
  r.lines.clear();
  fixed_perf::BeginView(&doc, "CBZ", "book.cbz", 0, 1, 400, 240);
  assert(!fixed_perf::NeedsPresentationTiming());
  fixed_perf::ViewStage("pdf.blit_main", 123);
  fake_tick += 500;
  fixed_perf::Drawn(2); // Interactive image ready; not presented yet.
  assert(fixed_perf::NeedsPresentationTiming());
  fixed_perf::Flush(&r);
  for (const auto &s : r.lines)
    assert(s.find("first_present") == std::string::npos);
  fake_tick += 1500;
  fixed_perf::Presented();
  fixed_perf::Flush(&r);
  int first = 0, final = 0;
  for (const auto &s : r.lines) {
    first += s.find("first_present") != std::string::npos;
    final += s.find("interactive_present") != std::string::npos;
  }
  assert(first == 1 && final == 1);
  bool timed = false;
  for (const auto &s : r.lines)
    timed |= s.find("stage=first_present us=2000 ") != std::string::npos;
  assert(timed);
  bool stage = false, tail = false;
  for (const auto &s : r.lines) {
    stage |= s.find("stage=pdf.blit_main us=123 ") != std::string::npos;
    tail |= s.find("stage=draw_to_present us=1500 ") != std::string::npos;
  }
  assert(stage && tail && !fixed_perf::NeedsPresentationTiming());
  auto n = r.lines.size();
  fixed_perf::ViewStage("pdf.blit_main", 999);
  fixed_perf::Drawn(2);
  assert(!fixed_perf::NeedsPresentationTiming());
  fixed_perf::Presented();
  fixed_perf::Flush(&r);
  assert(r.lines.size() == n); // No per-frame logging.
  fixed_perf::BeginView(&doc, "CBZ", "book.cbz", 1, 1, 400, 240);
  fixed_perf::Drawn(1);
  fixed_perf::Presented();
  fixed_perf::Flush(&r);
  assert(r.lines.size() > n);
  fixed_perf::ResetView();
  assert(!fixed_perf::NeedsPresentationTiming());
  std::thread worker([&] {
    for (int i = 0; i < 100; i++)
      fixed_perf::Record(&doc, 1, 1, "decode", 10, 1, 42, 20, 30);
  });
  std::thread worker2([&] {
    for (int i = 0; i < 100; i++)
      fixed_perf::Record(&doc, 2, 1, "prefetch_decode", 20, 1, 84, 40, 60);
  });
  worker.join();
  worker2.join();
  n = r.lines.size();
  fixed_perf::Flush(&r);
  assert(r.lines.size() > n &&
         r.lines.back().find("dropped=") != std::string::npos);
  puts(
      "PERF queue, presentation milestones, deduplication and overflow passed");
}
