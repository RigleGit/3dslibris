#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

typedef uint16_t u16;

// Pixel-backed Text substitute for the preview panel's real drawing boundary.
class Text {
public:
  int width, height, stride, color_mode, orientation;
  u16 *screenleft, *screenright, *screen;
  bool screenright_dirty, screenleft_dirty;
  uint64_t screenright_cache_generation;
  unsigned writes;
  std::vector<u16> bottom;

  Text(int w, int h, int o = 0)
      : width(w), height(h), stride(w + 3), color_mode(0), orientation(o),
        screenleft(NULL), screenright(NULL), screen(NULL),
        screenright_dirty(false), screenleft_dirty(false),
        screenright_cache_generation(0), writes(0),
        bottom((size_t)(w + 3) * h, 0xeeee) {
    screenright = bottom.data();
    screen = screenright;
  }
  int LogicalWidthFor(bool) const { return width; }
  int LogicalHeightFor(bool) const { return height; }
  int BufferStride() const { return stride; }
  int GetColorMode() const { return color_mode; }
  unsigned char GetOrientation() const { return orientation; }
  void SetScreen(u16 *value) { screen = value; }
  void MarkScreenDirty(u16 *) { screenright_dirty = true; ++writes; }
  void MarkScreenDirtyRect(u16 *, int, int, int, int) {
    screenright_dirty = true;
    ++writes;
  }
  void FillRect(u16 x0, u16 y0, u16 x1, u16 y1, u16 color) {
    MarkScreenDirtyRect(screen, x0, y0, x1, y1);
    for (int y = y0; y < std::min(height, (int)y1); ++y)
      for (int x = x0; x < std::min(width, (int)x1); ++x)
        bottom[(size_t)y * stride + x] = color;
  }
  void DrawRect(u16 x0, u16 y0, u16 x1, u16 y1, u16 color) {
    MarkScreenDirtyRect(screen, x0, y0, x1, y1);
    for (int y = y0; y < std::min(height, (int)y1); ++y)
      for (int x = x0; x < std::min(width, (int)x1); ++x)
        if (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1)
          bottom[(size_t)y * stride + x] = color;
  }
  void ClearScreen() { FillRect(0, 0, width, height, 0xffff); }
  bool HasDirtyScreens() const { return screenright_dirty || screenleft_dirty; }
  void Present() {
    if (screenright_dirty) ++screenright_cache_generation;
    screenright_dirty = false;
    screenleft_dirty = false;
  }
};
