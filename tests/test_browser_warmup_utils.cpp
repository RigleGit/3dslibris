#include "library/browser_warmup_utils.h"

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

void TestOld3dsWarmupQueueLimit() {
  ExpectTrue("old3ds selected cover queues when idle and queue empty",
             browser_warmup_utils::ShouldQueueCoverWarmupForDevice(
                 false, true, true, false, 0));
  ExpectFalse("old3ds skips speculative cover when queue busy",
              browser_warmup_utils::ShouldQueueCoverWarmupForDevice(
                  false, false, true, true, 1));
}

void TestOld3dsCoverMemoryGuard() {
  ExpectTrue("selected old3ds cover allowed above selected threshold",
             browser_warmup_utils::HasCoverExtractionHeadroom(
                 false, true,
                 browser_warmup_utils::kOld3dsSelectedCoverMinFreeBytes));
  ExpectFalse("selected old3ds cover blocked below selected threshold",
              browser_warmup_utils::HasCoverExtractionHeadroom(
                  false, true,
                  browser_warmup_utils::kOld3dsSelectedCoverMinFreeBytes - 1));
  ExpectFalse("warm old3ds cover blocked below warm threshold",
              browser_warmup_utils::HasCoverExtractionHeadroom(
                  false, false,
                  browser_warmup_utils::kOld3dsWarmCoverMinFreeBytes - 1));
}

void TestOld3dsCoverRetryBackoff() {
  ExpectTrue("selected old3ds retry delay applied",
             browser_warmup_utils::CoverRetryDelayMs(false, true, 4, false) ==
                 browser_warmup_utils::kOld3dsSelectedCoverRetryDelayMs);
  ExpectTrue("warm old3ds retry delay applied",
             browser_warmup_utils::CoverRetryDelayMs(false, false, 0, false) ==
                 browser_warmup_utils::kOld3dsWarmCoverRetryDelayMs);
  ExpectTrue("new3ds does not back off successful decode",
             browser_warmup_utils::CoverRetryDelayMs(true, true, 0, true) == 0);
}

} // namespace

int main() {
  TestWarmupBlockedDuringReleaseWait();
  TestWarmupRequiresIdleDelay();
  TestHeavyWarmupRequiresLongerIdleDelay();
  TestWarmupRejectsWrappedClock();
  TestSelectedCoverWarmupUsesShortIdle();
  TestNonSelectedCoverWarmupUsesHeavyIdle();
  TestOld3dsWarmupQueueLimit();
  TestOld3dsCoverMemoryGuard();
  TestOld3dsCoverRetryBackoff();
  return 0;
}
