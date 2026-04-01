#include "book/reflow_cache_save_utils.h"

#include <iostream>

namespace {

bool ExpectTrue(const char *label, bool value) {
  if (!value) {
    std::cerr << "FAIL: " << label << std::endl;
    return false;
  }
  return true;
}

bool ExpectFalse(const char *label, bool value) {
  if (value) {
    std::cerr << "FAIL: " << label << std::endl;
    return false;
  }
  return true;
}

} // namespace

int main() {
  using reflow_cache_save_utils::ShouldDeferAsyncOpenCacheSave;
  using reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose;

  bool ok = true;

  ok &= ExpectTrue("defer save during async open",
                   ShouldDeferAsyncOpenCacheSave(true, true));
  ok &= ExpectFalse("do not defer when save not requested",
                    ShouldDeferAsyncOpenCacheSave(false, true));
  ok &= ExpectFalse("do not defer in sync open",
                    ShouldDeferAsyncOpenCacheSave(true, false));

  ok &= ExpectTrue("flush deferred cache on close with pages",
                   ShouldFlushDeferredCacheSaveOnClose(true, false, 12));
  ok &= ExpectFalse("do not flush while async open still pending",
                    ShouldFlushDeferredCacheSaveOnClose(true, true, 12));
  ok &= ExpectFalse("do not flush without pending cache",
                    ShouldFlushDeferredCacheSaveOnClose(false, false, 12));
  ok &= ExpectFalse("do not flush empty page set",
                    ShouldFlushDeferredCacheSaveOnClose(true, false, 0));

  return ok ? 0 : 1;
}
