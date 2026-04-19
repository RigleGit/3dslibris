#include "formats/mobi/mobi_cleanup_policy.h"
#include "test_assert.h"

namespace {

void TestSkipsCleanupWhenNoMapAndNoWrapFix() {
  test::ExpectFalse("skip simple mobi cleanup",
                    mobi_cleanup_policy::ShouldRunPostNormalizeCleanup(false, false));
}

void TestKeepsCleanupWhenHtmlMapExists() {
  test::ExpectTrue("cleanup with html map",
                   mobi_cleanup_policy::ShouldRunPostNormalizeCleanup(true, false));
}

void TestKeepsCleanupWhenWrapFixEnabled() {
  test::ExpectTrue("cleanup with wrap fix",
                   mobi_cleanup_policy::ShouldRunPostNormalizeCleanup(false, true));
}

} // namespace

int main() {
  TestSkipsCleanupWhenNoMapAndNoWrapFix();
  TestKeepsCleanupWhenHtmlMapExists();
  TestKeepsCleanupWhenWrapFixEnabled();
  return 0;
}
