#include "library/browser_grid_geometry_utils.h"

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

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void TestRoundedRectContainsClipsCorners() {
  ExpectFalse("outside negative x clipped",
              browser_grid_geometry_utils::RoundedRectContains(-1, 0, 20, 30,
                                                               5));
  ExpectFalse("outside right edge clipped",
              browser_grid_geometry_utils::RoundedRectContains(20, 10, 20, 30,
                                                               5));
  ExpectFalse("sharp corner clipped",
              browser_grid_geometry_utils::RoundedRectContains(0, 0, 20, 30,
                                                               5));
  ExpectTrue("top edge center kept",
             browser_grid_geometry_utils::RoundedRectContains(10, 0, 20, 30,
                                                              5));
  ExpectTrue("left edge center kept",
             browser_grid_geometry_utils::RoundedRectContains(0, 15, 20, 30,
                                                              5));
  ExpectTrue("inside body kept",
             browser_grid_geometry_utils::RoundedRectContains(10, 15, 20, 30,
                                                              5));
}

void TestRoundedRectContainsHandlesDegenerateInputs() {
  ExpectFalse("empty width clipped",
              browser_grid_geometry_utils::RoundedRectContains(0, 0, 0, 10,
                                                               5));
  ExpectFalse("empty height clipped",
              browser_grid_geometry_utils::RoundedRectContains(0, 0, 10, 0,
                                                               5));
  ExpectTrue("radius zero behaves as rectangle",
             browser_grid_geometry_utils::RoundedRectContains(0, 0, 10, 10,
                                                              0));
}

void TestFitRectPreserveAspect() {
  int out_w = -1;
  int out_h = -1;
  browser_grid_geometry_utils::FitRectPreserveAspect(200, 100, 80, 80,
                                                     &out_w, &out_h);
  ExpectEq("wide image fills width", out_w, 80);
  ExpectEq("wide image preserves height", out_h, 40);

  browser_grid_geometry_utils::FitRectPreserveAspect(100, 200, 80, 80,
                                                     &out_w, &out_h);
  ExpectEq("tall image preserves width", out_w, 40);
  ExpectEq("tall image fills height", out_h, 80);

  browser_grid_geometry_utils::FitRectPreserveAspect(0, 100, 80, 80,
                                                     &out_w, &out_h);
  ExpectEq("invalid source width clears output width", out_w, 0);
  ExpectEq("invalid source width clears output height", out_h, 0);
}

} // namespace

int main() {
  TestRoundedRectContainsClipsCorners();
  TestRoundedRectContainsHandlesDegenerateInputs();
  TestFitRectPreserveAspect();
  return 0;
}
