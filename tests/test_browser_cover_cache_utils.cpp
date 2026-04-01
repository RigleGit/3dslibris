#include "library/browser_cover_cache_utils.h"

#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::fprintf(stderr, "%s\n", msg);
  std::exit(1);
}

void TestVisibleRangeClampsToBounds() {
  const browser_cover_cache_utils::VisibleRange range =
      browser_cover_cache_utils::ComputeVisibleRange(8, 10, 4);
  if (range.start != 8 || range.end != 10)
    Fail("visible range should clamp to total books");
}

void TestRangeContainsVisibleIndicesOnly() {
  const browser_cover_cache_utils::VisibleRange range =
      browser_cover_cache_utils::ComputeVisibleRange(4, 20, 4);
  if (!browser_cover_cache_utils::RangeContains(range, 4) ||
      !browser_cover_cache_utils::RangeContains(range, 7) ||
      browser_cover_cache_utils::RangeContains(range, 3) ||
      browser_cover_cache_utils::RangeContains(range, 8)) {
    Fail("range containment should match visible page");
  }
}

} // namespace

int main() {
  TestVisibleRangeClampsToBounds();
  TestRangeContainsVisibleIndicesOnly();
  return 0;
}
