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

void TestHeavyWarmupIdleForDevice() {
  struct Case {
    bool is_new_3ds;
    uint64_t elapsed_ms;
    bool expected_idle;
  };
  const Case cases[] = {
      {false, 599, false}, {false, 600, false},
      {false, 1199, false}, {false, 1200, true},
      {true, 599, false}, {true, 600, true},
      {true, 1199, true}, {true, 1200, true}};
  for (const Case &row : cases) {
    const bool idle = browser_warmup_utils::IsBrowserHeavyWarmupIdleForDevice(
        row.is_new_3ds, 1000 + row.elapsed_ms, 1000, false);
    if (idle != row.expected_idle)
      Fail(std::string(row.is_new_3ds ? "New3DS" : "Old3DS") +
           " heavy warmup after " + std::to_string(row.elapsed_ms) + " ms");
  }
  for (bool is_new_3ds : {false, true}) {
    ExpectFalse("heavy warmup rejects clock rollback",
                browser_warmup_utils::IsBrowserHeavyWarmupIdleForDevice(
                    is_new_3ds, 999, 1000, false));
    ExpectFalse("release wait blocks heavy warmup after either delay",
                browser_warmup_utils::IsBrowserHeavyWarmupIdleForDevice(
                    is_new_3ds, 2200, 1000, true));
  }
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
  ExpectTrue("selected old3ds cover allowed below former conservative threshold",
             browser_warmup_utils::HasCoverExtractionHeadroom(
                 false, true, 20u * 1024u * 1024u));
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

void TestPdfCoverWarmupEnabledForAllDevices() {
  ExpectTrue("old3ds attempts pdf cover warmup",
             browser_warmup_utils::ShouldAttemptPdfCoverWarmup(false));
  ExpectTrue("new3ds attempts pdf cover warmup",
             browser_warmup_utils::ShouldAttemptPdfCoverWarmup(true));
}

void TestPermanentCoverFailureDetection() {
  ExpectTrue("rc=3 (password) is permanent",
             browser_warmup_utils::IsPermanentCoverFailure(3));
  ExpectTrue("rc=8 (too complex) is permanent",
             browser_warmup_utils::IsPermanentCoverFailure(8));
  ExpectFalse("rc=1 (generic) is not permanent",
              browser_warmup_utils::IsPermanentCoverFailure(1));
  ExpectFalse("rc=4 (open fail) is not permanent",
              browser_warmup_utils::IsPermanentCoverFailure(4));
  ExpectFalse("rc=0 (success) is not permanent",
              browser_warmup_utils::IsPermanentCoverFailure(0));
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
  TestHeavyWarmupIdleForDevice();
  TestWarmupRejectsWrappedClock();
  TestVisibleBrowserEntryCount();
  TestSelectedCoverWarmupUsesShortIdle();
  TestNonSelectedCoverWarmupUsesHeavyIdle();
  TestOld3dsWarmupQueueLimit();
  TestOld3dsCoverMemoryGuard();
  TestPdfCoverWarmupEnabledForAllDevices();
  TestPermanentCoverFailureDetection();
  TestOld3dsCoverRetryBackoff();
  TestWarmupCompletionState();
  return 0;
}
