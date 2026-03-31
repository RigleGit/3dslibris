#include "shared/browser_warmup_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void TestWarmupBlockedDuringReleaseWait() {
  ExpectFalse("release wait blocks warmup",
              browser_warmup_utils::IsBrowserWarmupIdle(1000, 0, true));
}

void TestWarmupRequiresIdleDelay() {
  ExpectFalse("below idle threshold",
              browser_warmup_utils::IsBrowserWarmupIdle(
                  browser_warmup_utils::kBrowserWarmupIdleDelayMs - 1, 0,
                  false));
  ExpectTrue("at idle threshold",
             browser_warmup_utils::IsBrowserWarmupIdle(
                 browser_warmup_utils::kBrowserWarmupIdleDelayMs, 0, false));
}

void TestHeavyWarmupRequiresLongerIdleDelay() {
  ExpectFalse("heavy below threshold",
              browser_warmup_utils::IsBrowserHeavyWarmupIdle(
                  browser_warmup_utils::kBrowserHeavyWarmupIdleDelayMs - 1, 0,
                  false));
  ExpectTrue("heavy at threshold",
             browser_warmup_utils::IsBrowserHeavyWarmupIdle(
                 browser_warmup_utils::kBrowserHeavyWarmupIdleDelayMs, 0,
                 false));
}

void TestWarmupRejectsWrappedClock() {
  ExpectFalse("wrapped clock rejected",
              browser_warmup_utils::IsBrowserWarmupIdle(50, 100, false));
}

void TestSelectedCoverWarmupUsesShortIdle() {
  ExpectTrue("selected cover uses short idle",
             browser_warmup_utils::ShouldQueueCoverWarmup(
                 true,
                 browser_warmup_utils::IsBrowserWarmupIdle(
                     browser_warmup_utils::kBrowserWarmupIdleDelayMs, 0, false),
                 false));
  ExpectFalse("selected cover still blocked before short idle",
              browser_warmup_utils::ShouldQueueCoverWarmup(true, false, true));
}

void TestNonSelectedCoverWarmupUsesHeavyIdle() {
  ExpectFalse("non-selected cover ignores short idle",
              browser_warmup_utils::ShouldQueueCoverWarmup(false, true, false));
  ExpectTrue("non-selected cover needs heavy idle",
             browser_warmup_utils::ShouldQueueCoverWarmup(false, true, true));
}

} // namespace

int main() {
  TestWarmupBlockedDuringReleaseWait();
  TestWarmupRequiresIdleDelay();
  TestHeavyWarmupRequiresLongerIdleDelay();
  TestWarmupRejectsWrappedClock();
  TestSelectedCoverWarmupUsesShortIdle();
  TestNonSelectedCoverWarmupUsesHeavyIdle();
  return 0;
}
