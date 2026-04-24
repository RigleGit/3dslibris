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

void TestReleaseWaitCanBeForcedClearAfterTimeout() {
  ExpectFalse("release wait timeout ignores inactive state",
              browser_warmup_utils::ShouldForceClearInputRelease(2000, 0,
                                                                 false));
  ExpectFalse("release wait timeout waits below threshold",
              browser_warmup_utils::ShouldForceClearInputRelease(
                  browser_warmup_utils::kBrowserInputReleaseMaxWaitMs - 1, 0,
                  true));
  ExpectTrue("release wait timeout clears at threshold",
             browser_warmup_utils::ShouldForceClearInputRelease(
                 browser_warmup_utils::kBrowserInputReleaseMaxWaitMs, 0,
                 true));
  ExpectFalse("release wait timeout rejects wrapped clock",
              browser_warmup_utils::ShouldForceClearInputRelease(50, 100,
                                                                 true));
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

void TestVisibleBrowserEntryCount() {
  ExpectTrue("list full page visible count",
             browser_warmup_utils::VisibleBrowserEntryCount(7, 0, 12) == 7);
  ExpectTrue("list tail visible count",
             browser_warmup_utils::VisibleBrowserEntryCount(7, 7, 12) == 5);
  ExpectTrue("negative page start rejected",
             browser_warmup_utils::VisibleBrowserEntryCount(7, -1, 12) == 0);
  ExpectTrue("empty tail rejected",
             browser_warmup_utils::VisibleBrowserEntryCount(7, 12, 12) == 0);
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

void TestOld3dsPdfCoverWarmupIsDisabled() {
  ExpectFalse("old3ds skips pdf cover warmup",
              browser_warmup_utils::ShouldAttemptPdfCoverWarmup(false));
  ExpectTrue("new3ds allows pdf cover warmup",
             browser_warmup_utils::ShouldAttemptPdfCoverWarmup(true));
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

void TestWarmupCompletionState() {
  ExpectTrue("metadata unsupported treated done",
             browser_warmup_utils::MetadataWarmupDone(false, false));
  ExpectFalse("metadata pending when supported",
              browser_warmup_utils::MetadataWarmupDone(true, false));
  ExpectTrue("cover unsupported treated done",
             browser_warmup_utils::CoverWarmupDone(false, false, 0, 3));
  ExpectTrue("cover done by pixels",
             browser_warmup_utils::CoverWarmupDone(true, true, 0, 3));
  ExpectTrue("cover done by capped attempts",
             browser_warmup_utils::CoverWarmupDone(true, false, 3, 3));
  ExpectFalse("cover pending before attempts exhausted",
              browser_warmup_utils::CoverWarmupDone(true, false, 1, 3));
  ExpectTrue("book warmup done when both sides resolved",
             browser_warmup_utils::BookWarmupDone(true, true, true, false, 3,
                                                  3));
  ExpectFalse("book warmup pending when metadata missing",
              browser_warmup_utils::BookWarmupDone(true, false, true, false, 3,
                                                   3));
}

} // namespace

int main() {
  TestWarmupBlockedDuringReleaseWait();
  TestReleaseWaitCanBeForcedClearAfterTimeout();
  TestWarmupRequiresIdleDelay();
  TestHeavyWarmupRequiresLongerIdleDelay();
  TestWarmupRejectsWrappedClock();
  TestVisibleBrowserEntryCount();
  TestSelectedCoverWarmupUsesShortIdle();
  TestNonSelectedCoverWarmupUsesHeavyIdle();
  TestOld3dsWarmupQueueLimit();
  TestOld3dsCoverMemoryGuard();
  TestOld3dsPdfCoverWarmupIsDisabled();
  TestOld3dsCoverRetryBackoff();
  TestWarmupCompletionState();
  return 0;
}
