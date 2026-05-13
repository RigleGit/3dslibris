/*
 * Minimal Text stub for host integration tests.
 * Replaces ui/text.h when compiled with -I tests/stubs.
 * Returns fixed layout metrics so reflowable parsers can produce pages
 * without a real FreeType/framebuffer setup.
 */
#pragma once

#include <cstdint>
#include <string>
#include "3ds/types.h"
#include "shared/text_token_constants.h"

class IStatusReporter;

class Text {
public:
  int pixelsize;
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
  int GetColorMode() { return 0; }
  u16 GetFgColor() { return fgcolor; }
  void SetTextColorOverride(u16) {}
  void ClearTextColorOverride() {}

  // Reporter
  void SetReporter(IStatusReporter *) {}
  IStatusReporter *GetReporter() const { return nullptr; }
  void SetFontDir(const std::string &) {}

  // Pen / position
  void InitPen() {}
  u16 GetPenX() { return 0; }
  u16 GetPenY() { return 0; }
  void SetPen(u16, u16) {}

  // Screen management
  u16 *GetScreen() { return screen; }
  void SetScreen(u16 *s) { screen = s; }
  void MarkScreenDirty(u16 *) {}
  void MarkScreenDirtyRect(u16 *, int, int, int, int) {}
  void CopyScreen(u16 *, u16 *) {}

  // Drawing
  void FillRect(u16, u16, u16, u16, u16) {}
  bool PrintNewLine() { return false; }
  void ClearScreen() {}
  void PrintChar(u32) {}
  void PrintChar(u32, u8) {}
  void PrintString(const char *) {}
  void PrintString(const char *, u8) {}

  // Wrap / clip flags
  bool IsAutoWrapEnabled() const { return false; }
  void SetAutoWrapEnabled(bool) {}
  bool IsClipToContentEnabled() const { return false; }
  void SetClipToContentEnabled(bool) {}

  // Script scale (superscript/subscript)
  void SetScriptScale(float) {}
  float GetScriptScale() const { return 1.0f; }
};
