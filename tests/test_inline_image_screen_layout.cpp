#include "book/inline_image_screen_layout.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestLeftScreenGeometry() {
  const InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayout(true, 36);
  ExpectEq("left current height", layout.current_screen_height, 400);
  ExpectEq("left current bottom margin", layout.current_margin_bottom, 44);
  ExpectEq("left next height", layout.next_screen_height, 320);
  ExpectEq("left next bottom margin", layout.next_margin_bottom, 16);
}

void TestRightScreenGeometry() {
  const InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayout(false, 36);
  ExpectEq("right current height", layout.current_screen_height, 320);
  ExpectEq("right current bottom margin", layout.current_margin_bottom, 16);
  ExpectEq("right next height", layout.next_screen_height, 400);
  ExpectEq("right next bottom margin", layout.next_margin_bottom, 44);
}

void TestSmallFooterPreserved() {
  const InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayout(false, 10);
  ExpectEq("small current bottom margin", layout.current_margin_bottom, 10);
  ExpectEq("small next bottom margin", layout.next_margin_bottom, 18);
}

void TestReadingOrderDefaultOrientation() {
  InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayoutForReadingScreen(false, 0, 36);
  ExpectEq("default first reading screen height", layout.current_screen_height,
           400);
  ExpectEq("default first next height", layout.next_screen_height, 320);

  layout = ResolveInlineImageScreenLayoutForReadingScreen(false, 1, 36);
  ExpectEq("default second reading screen height", layout.current_screen_height,
           320);
  ExpectEq("default second next height", layout.next_screen_height, 400);
}

void TestReadingOrderTurnedRightOrientation() {
  InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayoutForReadingScreen(true, 0, 36);
  ExpectEq("turned first reading screen height", layout.current_screen_height,
           320);
  ExpectEq("turned first next height", layout.next_screen_height, 400);

  layout = ResolveInlineImageScreenLayoutForReadingScreen(true, 1, 36);
  ExpectEq("turned second reading screen height", layout.current_screen_height,
           400);
  ExpectEq("turned second next height", layout.next_screen_height, 320);
}

} // namespace

int main() {
  TestLeftScreenGeometry();
  TestRightScreenGeometry();
  TestSmallFooterPreserved();
  TestReadingOrderDefaultOrientation();
  TestReadingOrderTurnedRightOrientation();
  return 0;
}
