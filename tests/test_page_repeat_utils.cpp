#include "reader/page_repeat_utils.h"

#include "test_assert.h"

static void TestImmediatePressFiresOnce() {
  reader::PageRepeatState state;
  test::ExpectTrue(
      "initial key down fires",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, true,
                                   true, 1000, 400, 150));
  test::ExpectFalse(
      "same held key waits for initial delay",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false,
                                   true, 1200, 400, 150));
}

static void TestHoldRepeatsAfterDelayAndInterval() {
  reader::PageRepeatState state;
  reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, true, true,
                               1000, 400, 150);
  test::ExpectTrue(
      "held key fires after delay",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false,
                                   true, 1400, 400, 150));
  test::ExpectFalse(
      "held key waits for interval",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false,
                                   true, 1490, 400, 150));
  test::ExpectTrue(
      "held key fires after interval",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false,
                                   true, 1550, 400, 150));
}

static void TestReleaseAndDirectionChangeReset() {
  reader::PageRepeatState state;
  reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, true, true,
                               1000, 400, 150);
  reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false, false,
                               1100, 400, 150);
  test::ExpectFalse("release resets state",
                    state.action != reader::PAGE_REPEAT_NONE);

  reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, true, true,
                               1200, 400, 150);
  test::ExpectTrue("opposite direction fires immediately",
                   reader::ShouldFirePageRepeat(
                       &state, reader::PAGE_REPEAT_PREVIOUS, true, true, 1250,
                       400, 150));
  test::ExpectEq("direction changed", (int)state.action,
                 (int)reader::PAGE_REPEAT_PREVIOUS);
}

static void TestResetWhileHeldDoesNotFireImmediately() {
  reader::PageRepeatState state;
  reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, true, true,
                               1000, 400, 150);
  reader::ResetPageRepeat(&state);
  test::ExpectFalse(
      "held key after reset waits for delay",
      reader::ShouldFirePageRepeat(&state, reader::PAGE_REPEAT_NEXT, false,
                                   true, 1100, 400, 150));
}

int main() {
  TestImmediatePressFiresOnce();
  TestHoldRepeatsAfterDelayAndInterval();
  TestReleaseAndDirectionChangeReset();
  TestResetWhileHeldDoesNotFireImmediately();
  return 0;
}
