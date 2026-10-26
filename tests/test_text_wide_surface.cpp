#include "ui/text.h"
#include "ui/screen_constants.h"
#include "shared/color_utils.h"
#include "formats/common/fixed_layout_blit_utils.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

int test_model = 5;
Result test_cfg_result = 0;
bool test_wide = false;
u8 test_top_buffers[2][240 * 800 * 3];
u8 test_bottom_buffers[2][240 * 320 * 3];
int test_frame_slot = 0;
static int fail_surface_allocation_after = -1;

void *operator new[](size_t bytes, const std::nothrow_t &) noexcept {
  if (fail_surface_allocation_after == 0) {
    fail_surface_allocation_after = -1;
    return nullptr;
  }
  if (fail_surface_allocation_after > 0)
    --fail_surface_allocation_after;
  return ::operator new[](bytes);
}

// Font loading is outside this surface test. Rendering, ownership, geometry,
// mode changes and fixed-layout bitmap scaling use the production owners.
FontManager::FontManager(Text *) {}
FontManager::~FontManager() {}
#include "renderer_accessors.inc"

static void TestNativeDetailAndUiAcrossOrientations() {
  for (u8 orientation : {0, 1, 2}) {
    Text text;
    text.SetOrientation(orientation);
    u16 *left = text.screenleft, *right = text.screenright, *offscreen = text.offscreen;
    text.SetScreen(text.screenright);
    assert(text.SetTopScreenWideMode(true));
    assert(test_wide && text.GetScreen() == right);
    assert(text.screenleft == left && text.offscreen == offscreen);
    assert(text.BufferStride() == 400 && text.LogicalHeightFor(false) == (orientation == 2 ? 240 : 320));
    text.SetScreen(text.screenleft);
    text.ClearScreen();
    // Alternating samples along the physical horizontal axis prove native
    // detail survives; merely doubling a 400px bitmap cannot pass this.
    const int width = orientation == 2 ? 800 : 240;
    const int height = orientation == 2 ? 240 : 800;
    std::vector<u16> pixels(width * height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        pixels[y * width + x] = ((orientation == 2 ? x : y) & 1) ? 0x001F : 0xF800;
    // Separate strips must retain each other's native samples.
    for (int strip = 0; strip < 2; ++strip)
      fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(&text, text.screenleft,
          text.LogicalHeightFor(true), 0, strip * text.LogicalHeightFor(true) / 2,
          text.LogicalWidthFor(true), text.LogicalHeightFor(true) / 2,
          pixels, width, height, 0, strip * height / 2, width, height / 2, false);
    std::fill(test_bottom_buffers[0], test_bottom_buffers[0] + 240 * 320 * 3, 0xA5);
    assert(text.BlitToFramebuffer());
    for (int column = 0; column < 800; ++column) {
      const bool blue = orientation == 0 ? !(column & 1) : (column & 1);
      const int offset = (column * 240 + 100) * 3;
      assert(test_top_buffers[0][offset] == (blue ? 0xF8 : 0));
      assert(test_top_buffers[0][offset + 2] == (blue ? 0 : 0xF8));
    }
    // A normal UI rectangle overwrites both density samples, without changing
    // the rest of the native image. Bottom drawing never uses the wide stride.
    text.FillRect(5, 7, 6, 8, 0x07E0);
    text.SetScreen(text.screenright);
    text.FillRect(0, 0, 12, 12, 0xF800);
    assert(text.BlitToFramebuffer());
    for (int sample = 0; sample < 2; ++sample) {
      const int column = orientation == 2 ? 10 + sample
          : orientation == 1 ? 14 + sample : 785 - sample;
      const int row = orientation == 2 ? 232 : orientation == 1 ? 5 : 234;
      assert(test_top_buffers[0][(column * 240 + row) * 3 + 1] == 0xFC);
    }
    // A presentation invalidation (HOME/resume) must keep native detail and
    // the UI overlay. It does not mean the underlying pixels changed.
    text.MarkAllScreensDirty();
    assert(text.BlitToFramebuffer());
    const int untouched = (700 * 240 + 100) * 3;
    const bool untouched_blue = orientation == 0;
    assert(test_top_buffers[0][untouched] == (untouched_blue ? 0xF8 : 0));
    assert(test_top_buffers[0][untouched + 2] == (untouched_blue ? 0 : 0xF8));
    // Alternating hardware framebuffers must both receive the latest image.
    test_frame_slot = 1;
    assert(text.BlitToFramebuffer());
    assert(std::equal(test_top_buffers[0], test_top_buffers[0] + 240 * 800 * 3,
                      test_top_buffers[1]));
    test_frame_slot = 0;
    assert(text.SetTopScreenWideMode(false));
    assert(!test_wide && text.screenleft == left && text.screenright == right);
    assert(text.GetScreen() == right);
    assert(text.BlitToFramebuffer());
  }
}

static void TestCapabilityAndFailedAllocation() {
  test_model = CFG_MODEL_2DS;
  { Text text; assert(!text.SetTopScreenWideMode(true)); assert(!test_wide); }
  test_model = 5;
  test_cfg_result = -1;
  { Text text; assert(!text.SetTopScreenWideMode(true)); assert(!test_wide); }
  test_cfg_result = 0;
  Text text;
  u16 *left = text.screenleft;
  for (int allocation : {0, 1}) {
    fail_surface_allocation_after = allocation;
    assert(!text.SetTopScreenWideMode(true));
    assert(!test_wide && !text.IsWideTopScreenActive() && text.screenleft == left);
  }
  assert(text.SetTopScreenWideMode(true));
  assert(text.SetTopScreenWideMode(true));
  assert(text.SetTopScreenWideMode(false));
}

int main() {
  TestNativeDetailAndUiAcrossOrientations();
  TestCapabilityAndFailedAllocation();
  puts("PASS: native wide detail, UI, framebuffer swaps, capability and allocation rollback");
}
