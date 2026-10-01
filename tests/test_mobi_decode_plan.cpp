#include "formats/mobi/mobi_decode_plan.h"
#include "test_assert.h"

#include <limits>

int main() {
  // The 1 MiB cutoff is a memory/latency policy. Use literal boundary inputs,
  // so changing the cutoff or its inclusivity cannot change the oracle.
  const size_t sizes[] = {0, 1048575, 1048576, 1048577,
                          std::numeric_limits<size_t>::max()};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const mobi_decode_plan::Plan plan = mobi_decode_plan::Build(sizes[i]);
    test::ExpectEq("deferred finalization at 1 MiB", plan.defer_toc_finalize,
                   i >= 2);
    test::ExpectTrue("initial pass retains TOC metadata", plan.capture_toc_metadata);
    test::ExpectFalse("full markup buffer is released", plan.retain_markup_utf8);
  }
  return 0;
}
