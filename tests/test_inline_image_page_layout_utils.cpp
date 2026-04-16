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
      240, 400, 12, 12, 10, 36, 600, 1200, 2);
  ExpectEq("avail width excludes margins", p.avail_width, 212);
  ExpectEq("avail height excludes ui", p.avail_height, 350);
  ExpectTrue("draw height fits content box", p.draw_height <= p.avail_height);
  ExpectTrue("top respects top margin", p.start_y >= 12);
  ExpectTrue("bottom stays above footer",
             p.start_y + p.draw_height <= 362);
}

void TestSmallImageCentersWithinContentBox() {
  const InlineImagePagePlacement p = ResolveInlineImagePagePlacement(
      240, 320, 12, 12, 10, 16, 80, 60, 2);
  ExpectEq("no upscale width", p.draw_width, 80);
  ExpectEq("no upscale height", p.draw_height, 60);
  ExpectTrue("x centered", p.start_x > 14);
  ExpectTrue("y centered", p.start_y > 12);
}

} // namespace

int main() {
  TestPlacementUsesContentArea();
  TestSmallImageCentersWithinContentBox();
  return 0;
}
