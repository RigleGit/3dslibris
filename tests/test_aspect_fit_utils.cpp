#include "shared/aspect_fit_utils.h"

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

void TestSquareFitsWideBox() {
  const aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(0, 0, 200, 100, 100, 100, true);
  ExpectEq("square width", p.width, 100);
  ExpectEq("square height", p.height, 100);
  ExpectEq("square centered x", p.x, 50);
  ExpectEq("square y", p.y, 0);
}

void TestWideImageUpscalesAndCentersVertically() {
  const aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(0, 0, 200, 200, 100, 50, true);
  ExpectEq("wide width", p.width, 200);
  ExpectEq("wide height", p.height, 100);
  ExpectEq("wide x", p.x, 0);
  ExpectEq("wide centered y", p.y, 50);
}

void TestTallImageFitsHeightAndCentersHorizontally() {
  const aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(0, 0, 200, 200, 400, 800, true);
  ExpectEq("tall width", p.width, 100);
  ExpectEq("tall height", p.height, 200);
  ExpectEq("tall centered x", p.x, 50);
  ExpectEq("tall y", p.y, 0);
}

void TestNoUpscaleKeepsSmallSourceCentered() {
  const aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(0, 0, 200, 200, 50, 50, false);
  ExpectEq("small width", p.width, 50);
  ExpectEq("small height", p.height, 50);
  ExpectEq("small centered x", p.x, 75);
  ExpectEq("small centered y", p.y, 75);
}

void TestInvalidDimensionsClampSafely() {
  const aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(10, 20, 0, -5, 0, -10, true);
  ExpectEq("invalid width", p.width, 1);
  ExpectEq("invalid height", p.height, 1);
  ExpectEq("invalid x", p.x, 10);
  ExpectEq("invalid y", p.y, 20);
}

void TestNoUpscaleCoverThumbnailCompatibility() {
  aspect_fit_utils::Placement p =
      aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 50, 50, false);
  ExpectEq("small cover width", p.width, 50);
  ExpectEq("small cover height", p.height, 50);

  p = aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 85, 115, false);
  ExpectEq("exact cover width", p.width, 85);
  ExpectEq("exact cover height", p.height, 115);

  p = aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 1000, 500, false);
  ExpectEq("wide cover width", p.width, 85);
  ExpectEq("wide cover height", p.height, 42);

  p = aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 500, 1000, false);
  ExpectEq("tall cover width", p.width, 57);
  ExpectEq("tall cover height", p.height, 115);

  p = aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 1000, 1000, false);
  ExpectEq("square cover width", p.width, 85);
  ExpectEq("square cover height", p.height, 85);

  p = aspect_fit_utils::FitInsideBox(0, 0, 85, 115, 1600, 2400, false);
  ExpectEq("epub tall compatibility width", p.width, 76);
  ExpectEq("epub tall compatibility height", p.height, 115);
}

} // namespace

int main() {
  TestSquareFitsWideBox();
  TestWideImageUpscalesAndCentersVertically();
  TestTallImageFitsHeightAndCentersHorizontally();
  TestNoUpscaleKeepsSmallSourceCentered();
  TestInvalidDimensionsClampSafely();
  TestNoUpscaleCoverThumbnailCompatibility();
  return 0;
}
