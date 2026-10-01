#include "formats/common/fixed_layout_blit_utils.h"

#include "ui/text.h"
#include "shared/image_scale_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, u16 actual, u16 expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected equality");
}

u16 SourcePixel(int x, int y) {
  return (u16)(0x1000 + y * 0x0100 + x);
}

void TestExactCopyClipsNegativeOriginWithCropOffset() {
  Text ts;
  ts.display.width = 5;
  ts.display.height = 6;

  std::vector<u16> screen((size_t)ts.display.height * 4, 0xEEEE);
  std::vector<u16> pixels((size_t)6 * 5, 0);
  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 6; x++)
      pixels[(size_t)y * 6 + x] = SourcePixel(x, y);
  }

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 4, -1, -1, 4, 3, pixels, 6, 5, 1, 1, 4, 3, false);

  ExpectEq("left clipped row0 col0", screen[0 * 6 + 0], SourcePixel(2, 2));
  ExpectEq("left clipped row0 col1", screen[0 * 6 + 1], SourcePixel(3, 2));
  ExpectEq("left clipped row0 col2", screen[0 * 6 + 2], SourcePixel(4, 2));
  ExpectEq("left clipped row0 col3 untouched", screen[0 * 6 + 3], 0xEEEE);
  ExpectEq("left clipped row1 col0", screen[1 * 6 + 0], SourcePixel(2, 3));
  ExpectEq("left clipped row1 col2", screen[1 * 6 + 2], SourcePixel(4, 3));
  ExpectEq("unused stride col untouched", screen[0 * 6 + 5], 0xEEEE);
}

void TestExactCopyClipsRightEdgeWithCropOffset() {
  Text ts;
  ts.display.width = 5;
  ts.display.height = 6;

  std::vector<u16> screen((size_t)ts.display.height * 4, 0xEEEE);
  std::vector<u16> pixels((size_t)6 * 5, 0);
  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 6; x++)
      pixels[(size_t)y * 6 + x] = SourcePixel(x, y);
  }

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 4, 3, 1, 4, 2, pixels, 6, 5, 1, 2, 4, 2, false);

  ExpectEq("right clipped row0 col3", screen[1 * 6 + 3], SourcePixel(1, 2));
  ExpectEq("right clipped row0 col4", screen[1 * 6 + 4], SourcePixel(2, 2));
  ExpectEq("right clipped row0 stride untouched", screen[1 * 6 + 5], 0xEEEE);
  ExpectEq("right clipped row1 col3", screen[2 * 6 + 3], SourcePixel(1, 3));
  ExpectEq("right clipped row1 col4", screen[2 * 6 + 4], SourcePixel(2, 3));
}

std::vector<u16> CroppedColorCard() {
  // A two-by-two black/red/green/blue card surrounded by white pixels.
  return {0xffff, 0xffff, 0xffff, 0xffff,
          0xffff, 0x0000, 0xf800, 0xffff,
          0xffff, 0x07e0, 0x001f, 0xffff,
          0xffff, 0xffff, 0xffff, 0xffff};
}

void TestNearestResizeUsesCropAndPreservesDestinationBorder() {
  Text ts;
  ts.display.width = 5;
  ts.display.height = 6;
  std::vector<u16> screen(6 * 5, 0xeeee);
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 5, 1, 1, 3, 3, CroppedColorCard(),
      4, 4, 1, 1, 2, 2, false);
  // Nearest sampling repeats the first row/column when enlarging 2x2 to 3x3.
  const u16 expected[] = {0x0000, 0x0000, 0xf800,
                          0x0000, 0x0000, 0xf800,
                          0x07e0, 0x07e0, 0x001f};
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 6; ++x) {
      const u16 value = x >= 1 && x <= 3 && y >= 1 && y <= 3
          ? expected[(y - 1) * 3 + x - 1] : 0xeeee;
      ExpectEq("nearest resized card and untouched border", screen[y * 6 + x], value);
    }
  }
}

void TestBilinearResizeBlendsChannelsAndClipsWithoutShiftingSamples() {
  Text ts;
  ts.display.width = 3;
  ts.display.height = 4;
  std::vector<u16> screen(4 * 3, 0xeeee);
  // Midpoints are averages of the RGB channels: the center averages all four
  // corners to RGB565 0x4208. Expected values are literal analytic colors.
  const u16 expected[] = {0x0000, 0x8000, 0xf800,
                          0x0400, 0x4208, 0x8010,
                          0x07e0, 0x0410, 0x001f};
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 3, 0, 0, 3, 3, CroppedColorCard(),
      4, 4, 1, 1, 2, 2, true);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x)
      ExpectEq("bilinear analytic color", screen[y * 4 + x], expected[y * 3 + x]);
    ExpectEq("bilinear preserves stride padding", screen[y * 4 + 3], 0xeeee);
  }

  screen.assign(4 * 3, 0xeeee);
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 3, -1, -1, 3, 3, CroppedColorCard(),
      4, 4, 1, 1, 2, 2, true);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      const u16 value = x < 2 && y < 2 ? expected[(y + 1) * 3 + x + 1] : 0xeeee;
      ExpectEq("clipping keeps original resized sampling grid", screen[y * 4 + x], value);
    }
  }
}

void TestCropClampsToSourceEdgeAndInvalidBitmapDoesNotWrite() {
  Text ts;
  ts.display.width = 3;
  ts.display.height = 4;
  const std::vector<u16> pixels = {0x0000, 0xf800, 0x07e0, 0x001f};
  std::vector<u16> screen(4 * 2, 0xeeee);
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 2, 0, 0, 3, 2, pixels,
      2, 2, 99, 99, 10, 10, true);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 3; ++x)
      ExpectEq("out-of-range crop clamps to blue corner", screen[y * 4 + x], 0x001f);
    ExpectEq("clamped crop preserves padding", screen[y * 4 + 3], 0xeeee);
  }

  screen.assign(4 * 2, 0xeeee);
  const std::vector<u16> incomplete = {0x0000, 0xf800, 0x07e0};
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 2, 0, 0, 3, 2, incomplete,
      2, 2, 0, 0, 2, 2, false);
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 2, 0, 0, 0, 2, pixels,
      2, 2, 0, 0, 2, 2, false);
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      &ts, screen.data(), 2, 4, 0, 3, 2, pixels,
      2, 2, 0, 0, 2, 2, true);
  for (u16 pixel : screen)
    ExpectEq("invalid or invisible requests leave the screen untouched", pixel, 0xeeee);
}

void TestCoverDownsampleAveragesRgb888Channels() {
  const unsigned char rgb[] = {
      255, 0, 0, 0, 255, 0,
      0, 0, 255, 255, 255, 255,
  };
  u16 pixel = 0;
  if (!image_scale_utils::ScaleRgbToRgb565Bilinear(rgb, 2, 2, 1, 1, &pixel))
    Fail("cover downsample failed");
  // Each averaged channel rounds to 128, packed as R5=16, G6=32, B5=16.
  ExpectEq("cover averages all four RGB888 colors", pixel, 0x8410);
}

} // namespace

int main() {
  TestCoverDownsampleAveragesRgb888Channels();
  TestExactCopyClipsNegativeOriginWithCropOffset();
  TestExactCopyClipsRightEdgeWithCropOffset();
  TestNearestResizeUsesCropAndPreservesDestinationBorder();
  TestBilinearResizeBlendsChannelsAndClipsWithoutShiftingSamples();
  TestCropClampsToSourceEdgeAndInvalidBitmapDoesNotWrite();
  return 0;
}
