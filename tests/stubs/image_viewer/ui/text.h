#pragma once
#include <3ds.h>
#include "shared/text_token_constants.h"
#include "shared/text_unicode_utils.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// UI sink: fixed glyph metrics and captured draw calls. Button layout,
// clipping, hit testing and reader input dispatch are the production
// implementations.
class Text {
public:
  struct Printed {
    std::string text;
    int x, y;
    u16 *screen;
  };
  std::vector<Printed> printed;
  u16 left[400 * 240]{}, right[400 * 240]{};
  u16 *screenleft = left, *screenright = right, *screen = left;
  struct {
    int bottom = 36;
  } margin;
  bool landscape = false, wrap = true;
  int pixelsize = 14, style = 0, x = 0, y = 0;
  int GetPixelSize() const {
    return pixelsize;
  }
  void SetPixelSize(int size) {
    pixelsize = size;
  }
  int GetStyle() const {
    return style;
  }
  void SetStyle(int value) {
    style = value;
  }
  int GetHeight() const {
    return 14;
  }
  int GetAdvance(u32) const {
    return 7;
  }
  u8 GetCharCode(const char *s, u32 *cp) {
    return text_unicode_utils::DecodeNextDisplayCodepoint(s, std::strlen(s),
                                                          cp);
  }
  u8 GetCharCountInsideWidth(const char *s, u8, u8 width) {
    int count = 0, used = 0;
    while (*s) {
      u32 cp;
      int bytes = GetCharCode(s, &cp);
      if (!bytes || used + GetAdvance(cp) > width || count == 255)
        break;
      used += GetAdvance(cp);
      s += bytes;
      ++count;
    }
    return count;
  }
  bool IsAutoWrapEnabled() const {
    return wrap;
  }
  void SetAutoWrapEnabled(bool value) {
    wrap = value;
  }
  int LogicalWidthFor(bool top) const {
    return landscape ? (top ? 400 : 320) : 240;
  }
  int LogicalHeightFor(bool top) const {
    return landscape ? 240 : top ? 400 : 320;
  }
  int BufferStride() const {
    return 400;
  }
  u16 GetBgColor() const {
    return 0;
  }
  u16 GetFgColor() const {
    return 1;
  }
  u16 *GetScreen() const {
    return screen;
  }
  void SetScreen(u16 *value) {
    screen = value;
  }
  void SetPen(u16 px, u16 py) {
    x = px;
    y = py;
  }
  void PrintString(const char *s) {
    printed.push_back({s, x, y, screen});
  }
  void PrintString(const char *s, u8) {
    PrintString(s);
  }
  void ClearScreen() {}
  void DrawRect(int, int, int, int, u16) {}
  void MarkAllScreensDirty() {}
};
inline int ScreenHeightPx(const u16 *screen, const Text *ts) {
  return ts->LogicalHeightFor(screen == ts->screenleft);
}
