#include "ui/text.h"
#include "ui/screen_constants.h"
#include "shared/color_utils.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/fixed_layout_gpu_renderer.h"

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
static bool gpu_active = false;
static int gpu_restores = 0;
namespace fixed_layout_gpu {
bool Active(const Text *) { return gpu_active; }
void Software(Text *, bool discard) {
  if (gpu_active && !discard) ++gpu_restores;
  gpu_active = false;
}
void Shutdown(Text *) { gpu_active = false; }
}

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
    gpu_active = true;
    assert(!text.SetTopScreenWideMode(true));
    assert(!test_wide && !text.IsWideTopScreenActive() && text.screenleft == left);
    assert(gpu_active && "failed Wide activation must preserve the retained GPU image");
  }
  const int restores = gpu_restores;
  assert(text.SetTopScreenWideMode(true));
  assert(!gpu_active && gpu_restores == restores &&
         "successful full-screen geometry changes must avoid readback");
  assert(text.SetTopScreenWideMode(true));
  assert(text.SetTopScreenWideMode(false));
  gpu_active = true;
  text.SetOrientation(orientation_utils::ORIENT_LANDSCAPE);
  assert(!gpu_active && gpu_restores == restores);
}

int main() {
  // Independent physical corner oracle for transition-only GPU readback.
  const u16 expected[3][4]={{0x07e0,0xffff,0xf800,0x001f},
                          {0x001f,0xf800,0xffff,0x07e0},
                          {0xf800,0x07e0,0x001f,0xffff}};
  for (u8 orientation : {0,1,2}) for(bool wide : {false,true}) {
    Text text; text.SetOrientation(orientation);
    if(wide) assert(text.SetTopScreenWideMode(true));
    const int density=wide?2:1, physical_width=400*density;
    u8 *fb=test_top_buffers[0];
    std::fill(fb,fb+240*physical_width*3,0);
    const u16 colors[]={0xf800,0x07e0,0x001f,0xffff};
    for(int corner=0;corner<4;++corner) for(int sample=0;sample<density;++sample) {
      const int x=(corner&1)?physical_width-density+sample:sample;
      const int display_y=(corner&2)?239:0;
      const size_t off=((size_t)x*240+239-display_y)*3;
      const u16 pixel=colors[corner];
      fb[off]=(pixel&31)<<3; fb[off+1]=((pixel>>5)&63)<<2;
      fb[off+2]=((pixel>>11)&31)<<3;
    }
    text.RestoreGpuTop(fb,physical_width,orientation);
    const int w=text.LogicalWidthFor(true), h=text.LogicalHeightFor(true);
    const size_t offsets[]={0,(size_t)w-1,(size_t)(h-1)*400,(size_t)(h-1)*400+w-1};
    for(int corner=0;corner<4;++corner)
      assert(text.screenleft[offsets[corner]]==expected[orientation][corner]);
    if(wide) assert(text.SetTopScreenWideMode(false));
  }
  TestNativeDetailAndUiAcrossOrientations();
  TestCapabilityAndFailedAllocation();
  puts("PASS: native wide detail, UI, framebuffer swaps, capability and allocation rollback");
}
