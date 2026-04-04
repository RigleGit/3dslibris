/*
    3dslibris - text.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Central text renderer/typesetter backed by FreeType.
    - Manages dual software framebuffers and screen dirty-state tracking.
    - Exposes glyph caching, UTF-8 decoding, and UI/splash text primitives.
*/

#pragma once

#include <cstddef>
#include <3ds.h>
#include <string>
#include <vector>

#include "ui/framebuffer_blit_utils.h"
#include "ui/font_manager.h"
#include "ui/text_renderer.h"

//! Reference: FreeType2 online documentation
#define EMTOPIXEL (float)(POINTSIZE * DPI / 72.0)

//! Reference: http://www.displaymate.com/psp_ds_shootout.htm
#define DPI 110

// Parsing/rendering state
#define TEXT_BOLD_ON 2
#define TEXT_BOLD_OFF 3
#define TEXT_ITALIC_ON 4
#define TEXT_ITALIC_OFF 5
#define TEXT_IMAGE 6
#define TEXT_IMAGE_LEADING_PARAGRAPH 7
#define TEXT_IMAGE_FIGURE_WITH_CAPTION 8
#define TEXT_IMAGE_CONTEXT_DEFAULT 14

// Indices for filename and face vectors
#define TEXT_STYLE_REGULAR (u8)0
#define TEXT_STYLE_BOLD (u8)1
#define TEXT_STYLE_ITALIC (u8)2
#define TEXT_STYLE_BOLDITALIC (u8)3
#define TEXT_STYLE_BROWSER (u8)4

#define CACHESIZE 512

class App;

//! Typesetter singleton that provides all text rendering services.

//! Implemented atop FreeType 2.
//! Attempts to cache for draw performance using a homemade cache.
//! The code using FreeType's cache is inoperative.

class Text {

public:
  App *app;
  int pixelsize;
  struct {
    u8 r;
    u8 g;
    u8 b;
  } bgcolor;
  u16 fgcolor;
  bool usefgcolor;
  bool usebgcolor;
  //! Pointers to screens and which one is current.
  u16 *screen, *screenleft, *screenright;
  //! Offscreen buffer. Only used when OFFSCREEN defined.
  u16 *offscreen;
  std::vector<u8> screenleft_fb_cache;
  std::vector<u8> screenright_fb_cache;
  u64 screenleft_cache_generation;
  u64 screenright_cache_generation;
  framebuffer_blit_utils::PhysicalFramebufferSyncState screenleft_hw_sync;
  framebuffer_blit_utils::PhysicalFramebufferSyncState screenright_hw_sync;
  framebuffer_blit_utils::DirtyRect screenleft_dirty_rect;
  framebuffer_blit_utils::DirtyRect screenright_dirty_rect;
  struct {
    int left, right, top, bottom;
  } margin;
  struct {
    int width, height;
  } display;
  int linespacing;
  bool linebegan, bold, italic;
  bool screenleft_dirty, screenright_dirty;

  Text();
  Text(class App *parent) { app = parent; }
  ~Text();
  int Init();
  void InitPen(void);

  inline u8 GetAdvance(u32 ucs) { return GetAdvance(ucs, GetFace(GetStyle())); };
  inline u8 GetAdvance(u32 ucs, u8 astyle) { return GetAdvance(ucs, GetFace(astyle)); };
  u8 GetCharCode(const char *txt, u32 *code);
  u8 GetCharCode(const char *txt, size_t remaining, u32 *code);
  u8 GetCharCountInsideWidth(const char *txt, u8 style, u8 pixels);
  FT_Face GetFace();
  FT_Face GetFace(u8 astyle);
  std::string GetFontFile(u8 style);
  std::string GetFontName(u8 style);
  bool GetFontName(std::string &s);
  u8 GetHeight(void);
  bool GetInvert();
  void GetPen(u16 *x, u16 *y);
  void GetPen(u16 &x, u16 &y);
  u16 GetPenX();
  u16 GetPenY();
  u8 GetPixelSize();
  u16 *GetScreen();
  int GetStringAdvance(const char *txt);
  u8 GetStringWidth(const char *txt, u8 style);
  int GetStyle();

  void SetColorMode(int mode);
  int GetColorMode();
  void SetOrientation(bool turned_right);
  bool GetOrientation() const;
  void SetPen(u16 x, u16 y);
  void SetPixelSize(u8 size);
  void SetTextColorOverride(u16 color);
  void ClearTextColorOverride();
  void SetFace(u8 astyle);
  void SetFontFile(const char *path, u8 style);
  void SetScreen(u16 *s);
  void SetStyle(int astyle);
  void MarkScreenDirty(u16 *target);
  void MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1, int y1);
  void MarkCurrentScreenDirty();
  void MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1);
  void MarkAllScreensDirty();
  bool HasDirtyScreens() const;

  void ClearCache();
  void ClearCache(u8 style);
  void ClearRect(u16 xl, u16 yl, u16 xh, u16 yh);
  void FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color);
  void DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color);

  u16 GetFgColor();
  u16 GetBgColor();
  void ClearScreen();
  void ClearScreen(u16 *, u8, u8, u8);
  void CopyScreen(u16 *src, u16 *dst);
  bool BlitToFramebuffer();

  void PrintChar(u32 ucs);
  void PrintChar(u32 ucs, u8 style);
  bool PrintNewLine(void);
  void PrintString(const char *string);
  void PrintString(const char *string, u8 style);
  void PrintSplash(u16 *screen);

private:
  friend class FontManager;
  friend class TextRenderer;
  FontManager *fm;
  TextRenderer *tr;

  inline int CacheGlyph(u32 ucs) { return CacheGlyph(ucs, (u8)GetStyle()); }
  inline int CacheGlyph(u32 ucs, u8 style) {
    return CacheGlyph(ucs, GetFace(style));
  }
  int CacheGlyph(u32 ucs, FT_Face face);
  void ClearCache(FT_Face face);
  inline FT_GlyphSlot GetGlyph(u32 ucs, int flags) {
    return GetGlyph(ucs, flags, GetFace((u8)GetStyle()));
  };
  inline FT_GlyphSlot GetGlyph(u32 ucs, int flags, u8 astyle) {
    return GetGlyph(ucs, flags, GetFace(astyle));
  };
  FT_GlyphSlot GetGlyph(u32 ucs, int flags, FT_Face face);
  FT_Error GetGlyphBitmap(u32 ucs, FTC_SBit *asbit, FTC_Node *anode = NULL);
  FT_UInt GetGlyphIndex(u32 ucs);
  u8 GetAdvance(u32 ucs, FT_Face face);
  u8 GetStringWidth(const char *txt, FT_Face face);

  void PrintChar(u32 ucs, FT_Face face);
  void PrintString(const char *string, FT_Face face);
  void ReportFace(FT_Face face);
};
