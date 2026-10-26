#include "shared/text_screen_geometry.h"
#include "ui/framebuffer_blit_utils.h"
#include "ui/screen_layout_constants.h"
#include "ui/text_buffer_utils.h"
#include "ui/theme_colors.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

typedef unsigned char u8;
typedef unsigned int u32;

struct Text {
  std::vector<u16> left, right;
  u16 *screenleft, *screenright, *screen;
  unsigned char orientation;
  bool wide_top_valid_=false;
  framebuffer_blit_utils::DirtyRect wide_top_ui_dirty_{};
  bool screenleft_dirty=false, screenright_dirty=false;
  framebuffer_blit_utils::DirtyRect screenleft_dirty_rect{}, screenright_dirty_rect{};
  struct { int width, height; } display;
  Text() : left(400*400), right(400*400), screenleft(left.data()),
           screenright(right.data()), screen(screenleft), orientation(0) {
    display.width=240; display.height=400;
  }
  int BufferStride() const { return text_screen_geometry::kBufferStridePx; }
  int LogicalWidthFor(bool left_buffer) const {
    return text_screen_geometry::ResolveTextScreenGeometry(orientation, left_buffer).width;
  }
  int LogicalHeightFor(bool left_buffer) const {
    return text_screen_geometry::ResolveTextScreenGeometry(orientation, left_buffer).height;
  }
  int LogicalWidth() const { return LogicalWidthFor(screen == screenleft); }
  int LogicalHeight() const { return LogicalHeightFor(screen == screenleft); }
};
int ScreenHeightPx(const u16 *screen, const Text *text) {
  return text->LogicalHeightFor(screen == text->screenleft);
}

// Methods under test are extracted unchanged from the renderer owners. Only
// JPEG loading is replaced with a prepared portrait bitmap for PrintSplash.
class TextRenderer {
public:
  Text *parent;
  int colorMode;
  bool splash_light_attempted=false, splash_light_loaded=true;
  bool splash_dark_attempted=false, splash_dark_loaded=true;
  u16 *splash_light_pixels=nullptr, *splash_dark_pixels=nullptr;
  explicit TextRenderer(Text *text, int mode) : parent(text), colorMode(mode) {}
  void ClearScreen();
  void ClearRect(u16 xl, u16 yl, u16 xh, u16 yh);
  void MarkScreenDirty(u16 *target);
  void MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1, int y1);
  void MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1);
  u16 *GetScreen();
  void SetScreen(u16 *screen);
  int GetColorMode();
  void SetColorMode(int mode);
  bool EnsureSplashLoaded(bool dark) {
    return dark ? splash_dark_pixels != nullptr : splash_light_pixels != nullptr;
  }
  void DrawFallbackSplash() { assert(false && "prepared splash must be available"); }
  void PrintSplash(u16 *screen);
};
#include "text_surface_under_test.inc"

static const u16 kSentinel=0x1234;

static void TestGradientRectAndEndpoints() {
  Text text;
  for (int mode : {2, 5}) {
    TextRenderer renderer(&text, mode);
    // Changing geometry exercises cold/replaced caches, including the two
    // portrait heights and both 240px-high landscape buffers.
    for (unsigned char orientation : {0, 2, 1}) {
      text.orientation=orientation;
      for (bool left_buffer : {true, false}) {
        auto &pixels=left_buffer ? text.left : text.right;
        text.screen=left_buffer ? text.screenleft : text.screenright;
        std::fill(pixels.begin(), pixels.end(), kSentinel);
        text.screenleft_dirty_rect.valid=false;
        text.screenright_dirty_rect.valid=false;
        renderer.ClearRect(17, 33, 41, 72);
        const auto &dirty=left_buffer ? text.screenleft_dirty_rect : text.screenright_dirty_rect;
        assert(dirty.valid && dirty.x0 == 17 && dirty.y0 == 33 && dirty.x1 == 41 && dirty.y1 == 72);
        for (int y=0; y<400; ++y)
          for (int x=0; x<400; ++x)
            if (x<17 || x>=41 || y<33 || y>=72)
              assert(pixels[y*400+x] == kSentinel);
        const auto partial=pixels;
        renderer.ClearRect(41, 72, 17, 33);
        assert(pixels == partial);
        renderer.ClearScreen();
        for (int y=33; y<72; ++y)
          for (int x=17; x<41; ++x)
            assert(pixels[y*400+x] == partial[y*400+x]);
        for (int y=0; y<400; ++y)
          for (int x=0; x<400; ++x)
            if (y>=text.LogicalHeight() || x>=text.LogicalWidth())
              assert(pixels[y*400+x] == kSentinel);
        if (mode == 5 && orientation == 2 && left_buffer) {
          // The dark sepia endpoint is green=35 plus at most one dither
          // unit. A portrait-normalized gradient reaches green≈42 here.
          const int green=(pixels[239*400+200] >> 5) & 63;
          assert(green >= 8 && green <= 9);
        }
      }
    }
  }
}

static void TestSplashContainAndStride() {
  const u16 red=0xF800, blue=0x001F;
  std::vector<u16> portrait(240*400);
  std::fill(portrait.begin(), portrait.begin()+240*200, red);
  std::fill(portrait.begin()+240*200, portrait.end(), blue);
  for (unsigned char orientation : {0, 1, 2}) {
    for (int mode : {0, 1}) {
      Text text;
      text.orientation=orientation;
      text.screen=text.screenright;
      std::fill(text.left.begin(), text.left.end(), kSentinel);
      TextRenderer renderer(&text, mode);
      renderer.splash_light_pixels=portrait.data();
      renderer.splash_dark_pixels=portrait.data();
      renderer.PrintSplash(text.screenleft);
      assert(text.screen == text.screenright && renderer.colorMode == mode);
      assert(text.screenleft_dirty_rect.valid);
      const bool landscape=orientation == 2;
      const int x0=landscape ? 128 : 0, width=landscape ? 144 : 240;
      const int height=landscape ? 240 : 400;
      const u16 background=mode == 1 ? 0 : 0xFFFF;
      for (int y=0; y<400; ++y)
        for (int x=0; x<400; ++x) {
          u16 want=kSentinel;
          if (x<text.LogicalWidthFor(true) && y<text.LogicalHeightFor(true))
            want=(x>=x0 && x<x0+width) ? (y<height/2 ? red : blue) : background;
          assert(text.left[y*400+x] == want);
        }
    }
  }
}

int main() {
  TestGradientRectAndEndpoints();
  TestSplashContainAndStride();
  puts("PASS: gradient rect clears and splash respect logical screen geometry");
}
