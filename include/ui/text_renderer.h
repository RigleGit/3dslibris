#pragma once

#include <vector>

#include <3ds.h>

#include "ft2build.h"
#include FT_FREETYPE_H

#include "ui/framebuffer_blit_utils.h"

class Text;

class TextRenderer {
public:
  TextRenderer(Text *parent);
  ~TextRenderer();

  void PrintChar(u32 ucs);
  void PrintChar(u32 ucs, u8 style);
  void PrintChar(u32 ucs, FT_Face face);
  void PrintString(const char *s);
  void PrintString(const char *s, u8 style);
  void PrintString(const char *s, FT_Face face);
  bool PrintNewLine();

  void ClearScreen();
  void ClearScreen(u16 *screen, u8 r, u8 g, u8 b);
  void ClearRect(u16 xl, u16 yl, u16 xh, u16 yh);
  void FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color);
  void DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color);
  void CopyScreen(u16 *src, u16 *dst);

  u16 GetFgColor();
  u16 GetBgColor();
  void SetColorMode(int mode);
  int GetColorMode();
  void SetTextColorOverride(u16 color);
  void ClearTextColorOverride();

  void SetOrientation(bool right);
  bool GetOrientation() const;

  void GetPen(u16 *x, u16 *y);
  void GetPen(u16 &x, u16 &y);
  void SetPen(u16 x, u16 y);
  u16 GetPenX();
  u16 GetPenY();
  void InitPen();

  u16 *GetScreen();
  void SetScreen(u16 *s);

  void MarkScreenDirty(u16 *target);
  void MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1, int y1);
  void MarkCurrentScreenDirty();
  void MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1);
  void MarkAllScreensDirty();
  bool HasDirtyScreens() const;

  bool BlitToFramebuffer();

  void PrintSplash(u16 *screen);

  int GetDisplayWidth() const;
  int GetDisplayHeight() const;

  int GetStyle() const;
  void SetStyle(int s);
  int GetPixelSize() const;
  void SetPixelSizeVal(u8 s);
  bool GetFtc() const;
  u32 GetCodeprev() const;
  void SetCodeprev(u32 c);
  bool IsHit() const;
  void SetHit(bool h);
  bool IsLinebegan() const;
  void SetLinebegan(bool b);
  bool IsBold() const;
  void SetBold(bool b);
  bool IsItalic() const;
  void SetItalic(bool b);
  bool IsJustify() const;
  int GetLinespacing() const;
  bool IsAutoWrapEnabled() const;
  void SetAutoWrapEnabled(bool enabled);
  bool IsClipToContentEnabled() const;
  void SetClipToContentEnabled(bool enabled);

private:
  Text *parent;

  bool turned_right;
  int style;
  FT_Vector pen;
  u32 codeprev;
  bool hit;
  bool justify;
  int colorMode;

  bool splash_attempted;
  bool splash_loaded;
  u16 *splash_pixels;

  int stats_hits;
  int stats_misses;
  bool auto_wrap_enabled;
  bool clip_to_content_enabled;

  bool EnsureSplashLoaded();
  void DrawFallbackSplash();
};
