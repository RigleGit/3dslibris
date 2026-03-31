#include "shared/framebuffer_blit_utils.h"

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

void TestFramebufferSyncSkipsFreshCopies() {
  framebuffer_blit_utils::PhysicalFramebufferSyncState sync = {};
  unsigned char fb0[4] = {0};

  const int slot =
      framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb0);
  framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb0, 3);

  if (framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot, 3))
    Fail("fresh framebuffer generation should skip copy");
  if (!framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot, 4))
    Fail("stale framebuffer generation should require copy");
}

} // namespace

int main() {
  TestLogicalHeights();
  TestLogicalPixelCounts();
  TestConvertLogicalScreenToPhysicalCacheTurnedLeft();
  TestConvertLogicalScreenToPhysicalCacheTurnedRight();
  TestExpandDirtyRectClampsAndUnions();
  TestConvertLogicalRectToPhysicalCachePreservesOutsidePixels();
  TestFramebufferSyncSlotsReusePointers();
  TestFramebufferSyncSkipsFreshCopies();
  return 0;
}
