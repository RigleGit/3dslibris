#include "ui/framebuffer_blit_utils.h"
#include "ui/frame_debug_utils.h"
#include "shared/text_screen_geometry.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "app/status_layout_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectEqSize(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestLogicalHeights() {
  ExpectEq("left logical height",
           framebuffer_blit_utils::LogicalTextScreenHeight(true), 400);
  ExpectEq("right logical height",
           framebuffer_blit_utils::LogicalTextScreenHeight(false), 320);
}

void TestLogicalPixelCounts() {
  ExpectEqSize("left logical pixels",
               framebuffer_blit_utils::LogicalTextScreenPixelCount(240, true),
               (size_t)96000);
  ExpectEqSize("right logical pixels",
               framebuffer_blit_utils::LogicalTextScreenPixelCount(240, false),
               (size_t)76800);
}

void ExpectEqByte(const char *label, unsigned char actual,
                  unsigned char expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestConvertLogicalScreenToPhysicalCacheTurnedLeft() {
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 400);
  std::vector<unsigned short> logical((size_t)4 * (size_t)2, 0xFFFF);
  logical[0] = 0xF800;
  logical[1] = 0x07E0;
  logical[4] = 0x001F;

  std::vector<unsigned char> physical(geometry.byte_size, 0x00);
  framebuffer_blit_utils::ConvertLogicalRgb565ToPhysicalBgr888(
      physical.data(), geometry, logical.data(), 4, 2, 2, false);

  const size_t red_off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 0, false);
  ExpectEqByte("red b", physical[red_off + 0], 0x00);
  ExpectEqByte("red g", physical[red_off + 1], 0x00);
  ExpectEqByte("red r", physical[red_off + 2], 0xF8);

  const size_t green_off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 1, 0, false);
  ExpectEqByte("green b", physical[green_off + 0], 0x00);
  ExpectEqByte("green g", physical[green_off + 1], 0xFC);
  ExpectEqByte("green r", physical[green_off + 2], 0x00);

  const size_t blue_off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 1, false);
  ExpectEqByte("blue b", physical[blue_off + 0], 0xF8);
  ExpectEqByte("blue g", physical[blue_off + 1], 0x00);
  ExpectEqByte("blue r", physical[blue_off + 2], 0x00);
}

void TestConvertLogicalScreenToPhysicalCacheTurnedRight() {
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 320);
  std::vector<unsigned short> logical((size_t)4 * (size_t)2, 0xFFFF);
  logical[0] = 0x001F;

  std::vector<unsigned char> physical(geometry.byte_size, 0x00);
  framebuffer_blit_utils::ConvertLogicalRgb565ToPhysicalBgr888(
      physical.data(), geometry, logical.data(), 4, 2, 2, true);

  const size_t off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 0, true);
  ExpectEqByte("turned-right blue b", physical[off + 0], 0xF8);
  ExpectEqByte("turned-right blue g", physical[off + 1], 0x00);
  ExpectEqByte("turned-right blue r", physical[off + 2], 0x00);
}

void TestExpandDirtyRectClampsAndUnions() {
  framebuffer_blit_utils::DirtyRect dirty = {0, 0, 0, 0, false};
  framebuffer_blit_utils::ExpandDirtyRect(&dirty, -4, 5, 10, 20, 240, 320);
  if (!dirty.valid || dirty.x0 != 0 || dirty.y0 != 5 || dirty.x1 != 10 ||
      dirty.y1 != 20) {
    Fail("first dirty rect should clamp into bounds");
  }

  framebuffer_blit_utils::ExpandDirtyRect(&dirty, 8, 3, 30, 12, 240, 320);
  if (dirty.x0 != 0 || dirty.y0 != 3 || dirty.x1 != 30 || dirty.y1 != 20) {
    Fail("dirty rect should union with prior bounds");
  }
}

void TestConvertLogicalRectToPhysicalCachePreservesOutsidePixels() {
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 400);
  std::vector<unsigned short> logical((size_t)4 * (size_t)4, 0xFFFF);
  logical[1] = 0xF800;

  std::vector<unsigned char> physical(geometry.byte_size, 0xAA);
  const framebuffer_blit_utils::DirtyRect dirty =
      framebuffer_blit_utils::MakeDirtyRect(1, 0, 2, 1);
  framebuffer_blit_utils::ConvertLogicalRgb565RectToPhysicalBgr888(
      physical.data(), geometry, logical.data(), 4, 4, 4, false, dirty);

  const size_t red_off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 1, 0, false);
  ExpectEqByte("rect red b", physical[red_off + 0], 0x00);
  ExpectEqByte("rect red g", physical[red_off + 1], 0x00);
  ExpectEqByte("rect red r", physical[red_off + 2], 0xF8);

  const size_t untouched_off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 0, false);
  ExpectEqByte("outside rect preserved b", physical[untouched_off + 0], 0xAA);
  ExpectEqByte("outside rect preserved g", physical[untouched_off + 1], 0xAA);
  ExpectEqByte("outside rect preserved r", physical[untouched_off + 2], 0xAA);
}

void TestFramebufferSyncSlotsReusePointers() {
  framebuffer_blit_utils::PhysicalFramebufferSyncState sync = {};
  unsigned char fb0[4] = {0};
  unsigned char fb1[4] = {0};

  const int slot0 =
      framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb0);
  const int slot0_again =
      framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb0);
  const int slot1 =
      framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb1);

  ExpectEq("first pointer gets slot 0", slot0, 0);
  ExpectEq("same pointer reuses slot", slot0_again, 0);
  ExpectEq("second pointer gets slot 1", slot1, 1);
}

// The pre-landscape transform, kept as a golden reference: portrait offsets
// must never change or every existing left/right-handed user regresses.
size_t LegacyPortraitOffsetBytes(
    const framebuffer_blit_utils::FramebufferGeometry &geometry, int sx,
    int sy, bool turned_right) {
  const int dx = turned_right ? sy : (geometry.phys_width - 1 - sy);
  const int dy = turned_right ? sx : (geometry.stride - 1 - sx);
  return ((size_t)dx * (size_t)geometry.stride + (size_t)dy) * 3u;
}

void TestPortraitOffsetsMatchLegacyFormula() {
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 400);
  for (int sy = 0; sy < 400; sy += 7) {
    for (int sx = 0; sx < 240; sx += 7) {
      ExpectEqSize("turned-left golden",
                   framebuffer_blit_utils::PhysicalOffsetBytes(geometry, sx,
                                                               sy, 0),
                   LegacyPortraitOffsetBytes(geometry, sx, sy, false));
      ExpectEqSize("turned-right golden",
                   framebuffer_blit_utils::PhysicalOffsetBytes(geometry, sx,
                                                               sy, 1),
                   LegacyPortraitOffsetBytes(geometry, sx, sy, true));
    }
  }
}

void TestLandscapeOffsets() {
  // libctru framebuffers are column-major with the 240px axis as stride:
  // physical offset = (x * 240 + (239 - y)) * 3 for landscape coords (x, y).
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 400);
  ExpectEqSize("landscape origin",
               framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 0, 2),
               (size_t)239 * 3u);
  ExpectEqSize("landscape bottom-left",
               framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 0, 239, 2),
               (size_t)0);
  ExpectEqSize("landscape top-right",
               framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 399, 0, 2),
               ((size_t)399 * 240u + 239u) * 3u);
}

void TestOffsetsUniqueAndInBoundsPerOrientation() {
  const unsigned char orientations[] = {0, 1, 2};
  for (int left = 0; left < 2; ++left) {
    const framebuffer_blit_utils::FramebufferGeometry geometry =
        framebuffer_blit_utils::MakeFramebufferGeometry(240, left ? 400 : 320);
    for (unsigned char orientation : orientations) {
      const text_screen_geometry::ScreenGeometry logical =
          text_screen_geometry::ResolveTextScreenGeometry(orientation,
                                                          left != 0);
      const int width = logical.width;
      const int height = logical.height;
      std::vector<unsigned char> seen(geometry.byte_size / 3u, 0);
      for (int sy = 0; sy < height; sy++) {
        for (int sx = 0; sx < width; sx++) {
          const size_t off = framebuffer_blit_utils::PhysicalOffsetBytes(
              geometry, sx, sy, orientation);
          if (off % 3u != 0 || off + 2 >= geometry.byte_size)
            Fail("offset out of bounds for orientation " +
                 std::to_string((int)orientation));
          if (seen[off / 3u])
            Fail("duplicate physical pixel for orientation " +
                 std::to_string((int)orientation));
          seen[off / 3u] = 1;
        }
      }
    }
  }
}

void TestLandscapeConvertCoversFullWidth() {
  // Regression guard: the converter clamps used to cap logical x at the
  // 240px stride, which would drop the right 160px of a landscape top screen.
  const framebuffer_blit_utils::FramebufferGeometry geometry =
      framebuffer_blit_utils::MakeFramebufferGeometry(240, 400);
  const int stride = 400;
  std::vector<unsigned short> logical((size_t)stride * 240u, 0xFFFF);
  logical[(size_t)0 * stride + 399] = 0xF800; // top-right landscape pixel

  std::vector<unsigned char> physical(geometry.byte_size, 0x00);
  framebuffer_blit_utils::ConvertLogicalRgb565ToPhysicalBgr888(
      physical.data(), geometry, logical.data(), stride, 400, 240, 2);

  const size_t off =
      framebuffer_blit_utils::PhysicalOffsetBytes(geometry, 399, 0, 2);
  ExpectEqByte("landscape right edge b", physical[off + 0], 0x00);
  ExpectEqByte("landscape right edge g", physical[off + 1], 0x00);
  ExpectEqByte("landscape right edge r", physical[off + 2], 0xF8);
}

void TestFramebufferSyncSkipsFreshCopies() {
  framebuffer_blit_utils::PhysicalFramebufferSyncState sync = {};
  unsigned char fb0[4] = {0};

  const int slot =
      framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb0);
  framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb0, 3);

  const bool fresh_copy =
      framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot, 3);
  const bool stale_copy =
      framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot, 4);
  if (fresh_copy)
    Fail("fresh framebuffer generation should skip copy");
  if (!stale_copy)
    Fail("stale framebuffer generation should require copy");
  if (frame_debug_utils::ShouldLogBlitPage(false, fresh_copy))
    Fail("steady idle framebuffer should not log");
  if (!frame_debug_utils::ShouldLogBlitPage(false, stale_copy))
    Fail("stale framebuffer copy should log");
  if (!frame_debug_utils::ShouldLogBlitPage(true, fresh_copy))
    Fail("dirty page should log even with a fresh framebuffer");
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectGeometry(const char *label, unsigned char orientation,
                    bool is_left_buffer, int width, int height) {
  const text_screen_geometry::ScreenGeometry g =
      text_screen_geometry::ResolveTextScreenGeometry(orientation,
                                                      is_left_buffer);
  ExpectEq((std::string(label) + " width").c_str(), g.width, width);
  ExpectEq((std::string(label) + " height").c_str(), g.height, height);
}

void TestPortraitGeometry() {
  using namespace orientation_utils;
  ExpectGeometry("turned-left left screen", ORIENT_TURNED_LEFT, true, 240, 400);
  ExpectGeometry("turned-left right screen", ORIENT_TURNED_LEFT, false, 240,
                 320);
  ExpectGeometry("turned-right left screen", ORIENT_TURNED_RIGHT, true, 240,
                 400);
  ExpectGeometry("turned-right right screen", ORIENT_TURNED_RIGHT, false, 240,
                 320);
}

void TestLandscapeGeometry() {
  using namespace orientation_utils;
  ExpectGeometry("landscape top screen", ORIENT_LANDSCAPE, true, 400, 240);
  ExpectGeometry("landscape bottom screen", ORIENT_LANDSCAPE, false, 320, 240);
}

void TestGeometryFitsBufferStride() {
  using namespace orientation_utils;
  const unsigned char orientations[] = {ORIENT_TURNED_LEFT, ORIENT_TURNED_RIGHT,
                                        ORIENT_LANDSCAPE};
  for (unsigned char o : orientations) {
    for (int left = 0; left < 2; ++left) {
      const text_screen_geometry::ScreenGeometry g =
          text_screen_geometry::ResolveTextScreenGeometry(o, left != 0);
      const int max_index =
          (g.height - 1) * text_screen_geometry::kBufferStridePx +
          (g.width - 1);
      if (max_index >= text_screen_geometry::kBufferStridePx *
                           text_screen_geometry::kBufferStridePx)
        Fail("geometry exceeds square buffer");
      if (g.width > text_screen_geometry::kBufferStridePx)
        Fail("logical width exceeds buffer stride");
    }
  }
}

void TestOrientationPredicates() {
  using namespace orientation_utils;
  if (IsTurnedRight(ORIENT_TURNED_LEFT) ||
      !IsTurnedRight(ORIENT_TURNED_RIGHT) || IsTurnedRight(ORIENT_LANDSCAPE))
    Fail("IsTurnedRight must be true only for ORIENT_TURNED_RIGHT");
  if (IsLandscape(ORIENT_TURNED_LEFT) || IsLandscape(ORIENT_TURNED_RIGHT) ||
      !IsLandscape(ORIENT_LANDSCAPE))
    Fail("IsLandscape must be true only for ORIENT_LANDSCAPE");
  if (!FirstScreenIsLeft(ORIENT_TURNED_LEFT) ||
      FirstScreenIsLeft(ORIENT_TURNED_RIGHT) ||
      !FirstScreenIsLeft(ORIENT_LANDSCAPE))
    Fail("FirstScreenIsLeft must be false only for ORIENT_TURNED_RIGHT");
}

void TestFixedLayoutTargetDimensions() {
  fixed_layout_screen::TargetDimensions top = fixed_layout_screen::TargetDims(
      orientation_utils::ORIENT_LANDSCAPE, true);
  fixed_layout_screen::TargetDimensions bottom =
      fixed_layout_screen::TargetDims(orientation_utils::ORIENT_LANDSCAPE,
                                      false);
  if (top.width != 400 || top.height != 240 || bottom.width != 320 ||
      bottom.height != 240)
    Fail("fixed-layout landscape target dimensions mismatch");
}

void TestTopScreenLeavesBottomPadding() {
  status_layout_utils::BookStatusHudLayout layout =
      status_layout_utils::ComputeBookStatusHudLayout(
          text_screen_geometry::ResolveTextScreenGeometry(0, true).height, 12,
          36);
  ExpectTrue("text baseline inside screen", layout.text_y < 400);
  ExpectTrue("progress bar bottom padded",
             layout.progress_bar_y + layout.progress_bar_height <= 387);
  ExpectEq("clear band starts at reserved footer", layout.clear_top, 364);
  ExpectEq("clear band ends at screen bottom", layout.clear_bottom, 400);
}

void TestShorterScreenStillFits() {
  status_layout_utils::BookStatusHudLayout layout =
      status_layout_utils::ComputeBookStatusHudLayout(
          text_screen_geometry::ResolveTextScreenGeometry(0, false).height, 12,
          16);
  ExpectTrue("progress bar fits shorter screen",
             layout.progress_bar_y + layout.progress_bar_height <= 307);
  ExpectEq("clear band respects footer reserve", layout.clear_top, 304);
}

void TestFixedLayoutBottomOverlayFits() {
  status_layout_utils::FixedLayoutBottomHudLayout layout =
      status_layout_utils::ComputeFixedLayoutBottomHudLayout(
          text_screen_geometry::ResolveTextScreenGeometry(0, false).height, 12);
  ExpectEq("fixed layout top text y", layout.time_y, 10);
  ExpectEq("fixed layout top clear start", layout.time_clear_top, 0);
  ExpectEq("fixed layout top clear end", layout.time_clear_bottom, 18);
  ExpectEq("fixed layout bottom text y", layout.page_y, 298);
  ExpectEq("fixed layout bottom clear start", layout.page_clear_top, 289);
  ExpectEq("fixed layout bottom clear end", layout.page_clear_bottom, 306);
  ExpectEq("fixed layout right margin", layout.right_margin, 8);
}

void TestLandscapeBookHudUsesSlimBottomStrip() {
  status_layout_utils::LandscapeBookStatusHudLayout layout =
      status_layout_utils::ComputeLandscapeBookStatusHudLayout(
          text_screen_geometry::ResolveTextScreenGeometry(2, false).width,
          text_screen_geometry::ResolveTextScreenGeometry(2, false).height, 12);
  ExpectEq("landscape clear top", layout.clear_top, 218);
  ExpectEq("landscape clear bottom", layout.clear_bottom, 240);
  ExpectEq("landscape text baseline", layout.text_y, 235);
  ExpectEq("landscape left margin", layout.left_margin, 6);
  ExpectEq("landscape right margin", layout.right_margin, 6);
  ExpectTrue("landscape progress bar fits strip",
             layout.progress_bar_y >= layout.clear_top &&
                 layout.progress_bar_y + layout.progress_bar_height <=
                     layout.clear_bottom);
}

} // namespace

int main() {
  TestPortraitGeometry();
  TestLandscapeGeometry();
  TestGeometryFitsBufferStride();
  TestOrientationPredicates();
  TestFixedLayoutTargetDimensions();
  TestTopScreenLeavesBottomPadding();
  TestShorterScreenStillFits();
  TestFixedLayoutBottomOverlayFits();
  TestLandscapeBookHudUsesSlimBottomStrip();
  TestLogicalHeights();
  TestLogicalPixelCounts();
  TestConvertLogicalScreenToPhysicalCacheTurnedLeft();
  TestConvertLogicalScreenToPhysicalCacheTurnedRight();
  TestExpandDirtyRectClampsAndUnions();
  TestConvertLogicalRectToPhysicalCachePreservesOutsidePixels();
  TestPortraitOffsetsMatchLegacyFormula();
  TestLandscapeOffsets();
  TestOffsetsUniqueAndInBoundsPerOrientation();
  TestLandscapeConvertCoversFullWidth();
  TestFramebufferSyncSlotsReusePointers();
  TestFramebufferSyncSkipsFreshCopies();
  return 0;
}
