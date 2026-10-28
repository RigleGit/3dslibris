#include <cassert>
#include <cstdio>

#include "shared/home_button_guard.h"

int main() {
  // An inner operation must not re-enable HOME before its caller is ready.
  aptSetHomeAllowed(true);
  {
    HomeButtonGuard outer;
    assert(!aptIsHomeAllowed());
    {
      HomeButtonGuard inner;
      assert(!aptIsHomeAllowed());
    }
    assert(!aptIsHomeAllowed());
  }
  assert(aptIsHomeAllowed());

  // A caller may have disabled HOME before entering any guarded operation.
  aptSetHomeAllowed(false);
  { HomeButtonGuard guard; }
  assert(!aptIsHomeAllowed());
  puts("HOME guards preserve nested and pre-existing restrictions");
}
