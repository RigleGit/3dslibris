#include "library/browser_touch_gesture_utils.h"

#include <cstdio>
#include <cstdlib>

using namespace browser_touch_gesture;

static void Expect(const char *label, Action got, Action want) {
  if (got != want) {
    fprintf(stderr, "%s: got %d want %d\n", label, (int)got, (int)want);
    exit(1);
  }
}

int main() {
  { // short tap: select on down, open on release
    State s;
    Expect("tap down", Step(s, false, true, true, 3, 1000), ACTION_SELECT);
    Expect("tap held 100ms", Step(s, false, true, false, 3, 1100), ACTION_NONE);
    Expect("tap release", Step(s, false, false, false, -1, 1150), ACTION_OPEN);
    Expect("after open idle", Step(s, false, false, false, -1, 1200), ACTION_NONE);
  }
  { // hold: preview once at threshold, never open on release
    State s;
    Expect("hold down", Step(s, false, true, true, 2, 1000), ACTION_SELECT);
    Expect("hold before", Step(s, false, true, false, 2, 1399), ACTION_NONE);
    Expect("hold at threshold", Step(s, false, true, false, 2, 1400), ACTION_PREVIEW);
    Expect("hold again", Step(s, false, true, false, 2, 1500), ACTION_NONE);
    Expect("hold release", Step(s, false, false, false, -1, 1900), ACTION_NONE);
  }
  { // missed threshold frame: release after the threshold is still a hold
    State s;
    Step(s, false, true, true, 2, 1000);
    Expect("late release", Step(s, false, false, false, -1, 1700), ACTION_NONE);
  }
  { // drag off the cell and release elsewhere: cancel
    State s;
    Step(s, false, true, true, 1, 1000);
    Expect("drag off", Step(s, false, true, false, 2, 1100), ACTION_NONE);
    Expect("release other cell", Step(s, false, false, false, -1, 1150), ACTION_NONE);
  }
  { // drag off and back on: still cancelled
    State s;
    Step(s, false, true, true, 1, 1000);
    Step(s, false, true, false, -1, 1050);
    Step(s, false, true, false, 1, 1100);
    Expect("back on cell", Step(s, false, false, false, -1, 1150), ACTION_NONE);
  }
  { // drag off then hold past threshold: no preview
    State s;
    Step(s, false, true, true, 1, 1000);
    Step(s, false, true, false, 2, 1100);
    Expect("cancelled hold", Step(s, false, true, false, 2, 1600), ACTION_NONE);
  }
  { // touch held from the previous screen: guard blocks everything
    State s;
    Expect("guarded down", Step(s, true, true, true, 3, 1000), ACTION_NONE);
    Expect("guarded held", Step(s, true, true, false, 3, 1100), ACTION_NONE);
    Expect("guarded release", Step(s, true, false, false, -1, 1200), ACTION_NONE);
    // Guard cleared while the touch is still held without a down edge.
    Expect("held no edge", Step(s, false, true, false, 3, 1300), ACTION_NONE);
    Expect("release no open", Step(s, false, false, false, -1, 1350), ACTION_NONE);
  }
  { // guard arming mid-gesture cancels it
    State s;
    Step(s, false, true, true, 3, 1000);
    Step(s, true, true, false, 3, 1050);
    Expect("release after guard", Step(s, false, false, false, -1, 1100), ACTION_NONE);
  }
  { // down on empty area never starts a gesture
    State s;
    Expect("empty down", Step(s, false, true, true, -1, 1000), ACTION_NONE);
    Expect("empty release", Step(s, false, false, false, -1, 1050), ACTION_NONE);
  }
  { // missed release frame then a new down: restart, no stale open
    State s;
    Step(s, false, true, true, 1, 1000);
    Expect("new down", Step(s, false, true, true, 2, 1100), ACTION_SELECT);
    Expect("open second", Step(s, false, false, false, -1, 1150), ACTION_OPEN);
  }
  puts("PASS test_browser_touch_gesture");
  return 0;
}
