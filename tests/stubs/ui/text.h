/*
 * Minimal Text stub for host integration tests.
 * Replaces ui/text.h when compiled with -I tests/stubs.
 * Returns fixed layout metrics so reflowable parsers can produce pages
 * without a real FreeType/framebuffer setup.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "3ds/types.h"
#include "shared/text_token_constants.h"

class IStatusReporter;

class Text {
public:
  int pixelsize;
  int color_mode = 0;
  struct { u8 r, g, b; } bgcolor;
  u16 fgcolor;
  bool usefgcolor;
  bool usebgcolor;
  u16 *screen, *screenleft, *screenright, *offscreen;
  struct { int left, right, top, bottom; } margin;
  struct { int width, height; } display;
  int linespacing;
  bool linebegan, bold, italic;
  bool screenleft_dirty, screenright_dirty;

  // Opt-in recording for tests that exercise the real Page::Draw loop.
  bool capture_rendered_text = false;
  bool landscape = false;
  int pen_x = 0, pen_y = 0;
  std::string rendered_ascii;
  std::vector<std::string> printed_strings;
  int clipped_glyphs = 0;
  struct RenderedGlyph { u32 codepoint; int x, y; u16 *screen; };
  std::vector<RenderedGlyph> rendered_glyphs;

  Text()
      : pixelsize(14), fgcolor(0), usefgcolor(false), usebgcolor(false),
        screen(nullptr), screenleft(nullptr), screenright(nullptr),
        offscreen(nullptr), linespacing(1), linebegan(false), bold(false),
        italic(false), screenleft_dirty(true), screenright_dirty(true) {
    bgcolor.r = 15; bgcolor.g = 15; bgcolor.b = 15;
    margin.left = 12; margin.right = 12; margin.top = 10; margin.bottom = 36;
    display.width = 400; display.height = 240;
  }
  ~Text() {}

  // Layout metrics
  u8 GetHeight() const { return (u8)pixelsize; }
  u8 GetAdvance(u32) const { return (u8)(pixelsize * 8 / 14); }
  u8 GetAdvance(u32, u8) const { return (u8)(pixelsize * 8 / 14); }
  u8 GetPixelSize() const { return (u8)pixelsize; }
  void SetPixelSize(u8 size) { pixelsize = (int)size; }
  int GetStringAdvance(const char *) { return 0; }

  // Style
  void SetStyle(int) {}
  int GetStyle() const { return 0; }
  std::string GetFontFile(u8) const { return ""; }
  std::string GetFontFile(u8, int) const { return ""; }
  int GetColorMode() { return color_mode; }
  void SetColorMode(int value) { color_mode = value; }
  u16 GetFgColor() { return fgcolor; }
  void SetTextColorOverride(u16) {}
  void ClearTextColorOverride() {}

  // Reporter
  void SetReporter(IStatusReporter *) {}
  IStatusReporter *GetReporter() const { return nullptr; }
  void SetFontDir(const std::string &) {}

  // Pen / position
  void InitPen() {
    if (capture_rendered_text) {
      pen_x = margin.left;
      pen_y = margin.top + GetHeight();
    }
  }
  u16 GetPenX() { return (u16)pen_x; }
  u16 GetPenY() { return (u16)pen_y; }
  void SetPen(u16 x, u16 y) {
    if (capture_rendered_text) { pen_x = x; pen_y = y; }
  }

  // Geometry. The stub keeps display.* configurable per test, so the buffer
  // stride and logical width mirror it instead of the real fixed constants.
  int BufferStride() const { return display.height; }
  int LogicalWidthFor(bool is_left_buffer) const {
    return capture_rendered_text && landscape
        ? (is_left_buffer ? 400 : 320) : display.width;
  }
  int LogicalHeightFor(bool is_left_buffer) const {
    if (capture_rendered_text && landscape) return 240;
    return is_left_buffer ? 400 : 320;
  }
  int LogicalWidth() const { return LogicalWidthFor(screen == screenleft); }
  int LogicalHeight() const {
    return capture_rendered_text ? LogicalHeightFor(screen == screenleft)
                                 : display.height;
  }

  // Screen management
  u16 *GetScreen() { return screen; }
  void SetScreen(u16 *s) { screen = s; }
  void MarkScreenDirty(u16 *) {}
  void MarkScreenDirtyRect(u16 *, int, int, int, int) {}
  void CopyScreen(u16 *, u16 *) {}

  // Drawing
  void FillRect(u16, u16, u16, u16, u16) {}
  void DrawRect(u16, u16, u16, u16, u16) {}
  bool PrintNewLine() {
    if (!capture_rendered_text) return false;
    pen_x = margin.left;
    const int next_y = pen_y + GetHeight() + linespacing;
    if (next_y > LogicalHeight() - margin.bottom) {
      if (screen != screenleft) return false;
      screen = screenright;
      pen_y = margin.top + GetHeight();
    } else {
      pen_y = next_y;
    }
    return true;
  }
  void ClearScreen() {}
  void PrintChar(u32 c) {
    if (!capture_rendered_text) return;
    if (c >= 32)
      rendered_glyphs.push_back({c, pen_x, pen_y, screen});
    if (c > 32 && c < 127) {
      if (pen_y <= LogicalHeight() - margin.bottom &&
          pen_x + GetAdvance(c) <= LogicalWidth() - margin.right)
        rendered_ascii += (char)c;
      else
        clipped_glyphs++;
    }
    pen_x += GetAdvance(c);
  }
  void PrintChar(u32 c, u8) { PrintChar(c); }
  void PrintString(const char *value) { if (value) printed_strings.push_back(value); }
  void PrintString(const char *value, u8) { PrintString(value); }

  // Wrap / clip flags
  bool IsAutoWrapEnabled() const { return false; }
  void SetAutoWrapEnabled(bool) {}
  bool IsClipToContentEnabled() const { return false; }
  void SetClipToContentEnabled(bool) {}

  // Script scale (superscript/subscript)
  void SetScriptScale(float) {}
  float GetScriptScale() const { return 1.0f; }
};
