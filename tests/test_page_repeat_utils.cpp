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

static void TestResolveDirectionalRepeat() {
  const uint32_t N = 1, P = 2, X = 4;
  using reader::ResolveDirectionalRepeat;
  {
    reader::PageRepeatState st;
    test::ExpectEq("press next fires next",
                   (int)ResolveDirectionalRepeat(&st, N, N, N, P, 1000, 400, 150),
                   (int)reader::PAGE_REPEAT_NEXT);
    test::ExpectEq("held next repeats after delay",
                   (int)ResolveDirectionalRepeat(&st, 0, N, N, P, 1400, 400, 150),
                   (int)reader::PAGE_REPEAT_NEXT);
    test::ExpectEq("held next waits for interval",
                   (int)ResolveDirectionalRepeat(&st, 0, N, N, P, 1490, 400, 150),
                   (int)reader::PAGE_REPEAT_NONE);
    test::ExpectEq("held next repeats after interval",
                   (int)ResolveDirectionalRepeat(&st, 0, N, N, P, 1550, 400, 150),
                   (int)reader::PAGE_REPEAT_NEXT);
  }
  {
    reader::PageRepeatState st;
    test::ExpectEq("press prev fires previous",
                   (int)ResolveDirectionalRepeat(&st, P, P, N, P, 1000, 400, 150),
                   (int)reader::PAGE_REPEAT_PREVIOUS);
  }
  {
    reader::PageRepeatState st;
    ResolveDirectionalRepeat(&st, N, N, N, P, 1000, 400, 150);
    test::ExpectEq("other held key cancels repeat",
                   (int)ResolveDirectionalRepeat(&st, 0, N | X, N, P, 1400, 400, 150),
                   (int)reader::PAGE_REPEAT_NONE);
    test::ExpectEq("other held key resets state", (int)st.action,
                   (int)reader::PAGE_REPEAT_NONE);
  }
  {
    reader::PageRepeatState st;
    test::ExpectEq("key outside both masks does not fire",
                   (int)ResolveDirectionalRepeat(&st, X, X, N, P, 1000, 400, 150),
                   (int)reader::PAGE_REPEAT_NONE);
  }
  {
    reader::PageRepeatState st;
    ResolveDirectionalRepeat(&st, N, N, N, P, 1000, 400, 150);
    test::ExpectEq("release resets",
                   (int)ResolveDirectionalRepeat(&st, 0, 0, N, P, 1100, 400, 150),
                   (int)reader::PAGE_REPEAT_NONE);
    test::ExpectEq("new hold does not fire immediately",
                   (int)ResolveDirectionalRepeat(&st, 0, N, N, P, 1500, 400, 150),
                   (int)reader::PAGE_REPEAT_NONE);
  }
}

int main() {
  TestImmediatePressFiresOnce();
  TestHoldRepeatsAfterDelayAndInterval();
  TestReleaseAndDirectionChangeReset();
  TestResetWhileHeldDoesNotFireImmediately();
  TestResolveDirectionalRepeat();
  return 0;
}
