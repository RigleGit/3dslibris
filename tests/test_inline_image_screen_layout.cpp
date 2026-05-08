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
      ResolveInlineImageScreenLayout(true, 36, 10);
  ExpectEq("left current height", layout.current_screen_height, 400);
  ExpectEq("left current bottom margin", layout.current_margin_bottom, 36);
  ExpectEq("left next height", layout.next_screen_height, 320);
  ExpectEq("left next bottom margin", layout.next_margin_bottom, 20);
}

void TestRightScreenGeometry() {
  const InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayout(false, 36, 10);
  ExpectEq("right current height", layout.current_screen_height, 320);
  ExpectEq("right current bottom margin", layout.current_margin_bottom, 20);
  ExpectEq("right next height", layout.next_screen_height, 400);
  ExpectEq("right next bottom margin", layout.next_margin_bottom, 36);
}

void TestSmallFooterPreserved() {
  const InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayout(false, 10, 10);
  ExpectEq("small current bottom margin", layout.current_margin_bottom, 10);
  ExpectEq("small next bottom margin", layout.next_margin_bottom, 10);
}

void TestReadingOrderDefaultOrientation() {
  InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayoutForReadingScreen(false, 0, 36, 10);
  ExpectEq("default first reading screen height", layout.current_screen_height,
           400);
  ExpectEq("default first next height", layout.next_screen_height, 320);

  layout = ResolveInlineImageScreenLayoutForReadingScreen(false, 1, 36, 10);
  ExpectEq("default second reading screen height", layout.current_screen_height,
           320);
  ExpectEq("default second next height", layout.next_screen_height, 400);
}

void TestReadingOrderTurnedRightOrientation() {
  InlineImageScreenLayout layout =
      ResolveInlineImageScreenLayoutForReadingScreen(true, 0, 36, 10);
  ExpectEq("turned first reading screen height", layout.current_screen_height,
           320);
  ExpectEq("turned first next height", layout.next_screen_height, 400);

  layout = ResolveInlineImageScreenLayoutForReadingScreen(true, 1, 36, 10);
  ExpectEq("turned second reading screen height", layout.current_screen_height,
           400);
  ExpectEq("turned second next height", layout.next_screen_height, 320);
}

void TestReadingScreenIndexForPhysicalScreen() {
  ExpectEq("default left physical -> first reading screen",
           ResolveReadingScreenIndexForPhysicalScreen(false, true), 0);
  ExpectEq("default right physical -> second reading screen",
           ResolveReadingScreenIndexForPhysicalScreen(false, false), 1);
  ExpectEq("turned left physical -> second reading screen",
           ResolveReadingScreenIndexForPhysicalScreen(true, true), 1);
  ExpectEq("turned right physical -> first reading screen",
           ResolveReadingScreenIndexForPhysicalScreen(true, false), 0);
}

} // namespace

int main() {
  TestLeftScreenGeometry();
  TestRightScreenGeometry();
  TestSmallFooterPreserved();
  TestReadingOrderDefaultOrientation();
  TestReadingOrderTurnedRightOrientation();
  TestReadingScreenIndexForPhysicalScreen();
  return 0;
}
