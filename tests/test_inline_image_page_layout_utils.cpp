#include "book/inline_image_page_layout_utils.h"

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

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void TestPlacementUsesContentArea() {
  const InlineImagePagePlacement p = ResolveInlineImagePagePlacement(
      240, 400, 0, 0, 0, 36, 600, 1200, 0, 0, 0);
  ExpectEq("avail width uses full screen", p.avail_width, 240);
  ExpectEq("avail height excludes ui", p.avail_height, 364);
  ExpectTrue("draw height fits content box", p.draw_height <= p.avail_height);
  ExpectTrue("top may use full image area", p.start_y >= 0);
  ExpectTrue("bottom stays above footer",
             p.start_y + p.draw_height <= 364);
}

void TestSmallImageCentersWithinContentBox() {
  const InlineImagePagePlacement p = ResolveInlineImagePagePlacement(
      240, 320, 0, 0, 0, 16, 80, 60, 0, 0, 0);
  ExpectEq("page image upscales width", p.draw_width, 240);
  ExpectEq("page image upscales height", p.draw_height, 180);
  ExpectEq("x fills screen", p.start_x, 0);
  ExpectTrue("y centered", p.start_y > 0);
}

void TestMarginsAndPaddingReduceAvailableArea() {
  const InlineImagePagePlacement p = ResolveInlineImagePagePlacement(
      240, 320, 10, 20, 30, 40, 100, 100, 5, 7, 9);
  ExpectEq("avail width excludes margins and horizontal padding",
           p.avail_width, 200);
  ExpectEq("avail height excludes margins and vertical padding",
           p.avail_height, 234);
  ExpectEq("start x begins after left margin and padding", p.start_x, 15);
  ExpectTrue("start y respects top margin and padding", p.start_y >= 37);
}

void TestInvalidSourceDimensionsClampSafely() {
  const InlineImagePagePlacement p = ResolveInlineImagePagePlacement(
      240, 320, 0, 0, 0, 0, 0, -1, 0, 0, 0);
  ExpectEq("invalid source available width", p.avail_width, 240);
  ExpectEq("invalid source available height", p.avail_height, 320);
  ExpectEq("invalid source draw width", p.draw_width, 240);
  ExpectEq("invalid source draw height", p.draw_height, 240);
}

} // namespace

int main() {
  TestPlacementUsesContentArea();
  TestSmallImageCentersWithinContentBox();
  TestMarginsAndPaddingReduceAvailableArea();
  TestInvalidSourceDimensionsClampSafely();
  return 0;
}
