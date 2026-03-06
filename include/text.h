#pragma once

#include <3ds.h>
#include <map>
#include <string>

#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_CACHE_H

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

// Indices for filename and face vectors
#define TEXT_STYLE_REGULAR (u8)0
#define TEXT_STYLE_BOLD (u8)1
#define TEXT_STYLE_ITALIC (u8)2
#define TEXT_STYLE_BOLDITALIC (u8)3
#define TEXT_STYLE_BROWSER (u8)4

#define CACHESIZE 512

class App;
int asciiart();
const char *ErrorString(u8);

typedef struct TextFaceRec_ {
  char file_path[128];
  int face_index;
} TextFaceRec, *TextFace;

typedef struct TextCache_ {
  FTC_Manager manager;
  FTC_CMapCache cmap;
  FTC_ImageCache image;
  FTC_SBitCache sbit;
} TextCache;

//! Homemade glyph cache.

//! Fetches from FreeType can be expensive,
//! so keep advance and gylphs handy,
//! keyed by UCS codepoint.

class Cache {
public:
  //! Associates each glyph cache index (value)
  //! with its Unicode code point (key).
  std::map<u16, FT_GlyphSlot> cacheMap;
  //! ??
  u16 cachenext;

  Cache() { cachenext = 0; }
};

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
  bool usebgcolor;
  //! Pointers to screens and which one is current.
  u16 *screen, *screenleft, *screenright;
  //! Offscreen buffer. Only used when OFFSCREEN defined.
  u16 *offscreen;
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
  void Begin();
  void End();

  inline u8 GetAdvance(u32 ucs) { return GetAdvance(ucs, GetFace(style)); };
  inline u8 GetAdvance(u32 ucs, u8 astyle) {
    return GetAdvance(ucs, GetFace(astyle));
  };
  u8 GetCharCode(const char *txt, u32 *code);
  u8 GetCharCountInsideWidth(const char *txt, u8 style, u8 pixels);
  FT_Face GetFace() { return faces[style]; }
  FT_Face GetFace(u8 astyle) { return faces[astyle]; }
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
  inline int GetStyle() { return style; }

  void SetColorMode(int mode);
  int GetColorMode();
  void SetPen(u16 x, u16 y);
  void SetPixelSize(u8 size);
  inline void SetFace(u8 astyle) { style = astyle; };
  void SetFontFile(const char *path, u8 style);
  void SetScreen(u16 *s);
  inline void SetStyle(int astyle) { style = astyle; }
  void MarkScreenDirty(u16 *target);
  inline void MarkCurrentScreenDirty() { MarkScreenDirty(screen); }
  inline void MarkAllScreensDirty() {
    screenleft_dirty = true;
    screenright_dirty = true;
  }
  inline bool HasDirtyScreens() const {
    return screenleft_dirty || screenright_dirty;
  }

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
  void SwapScreens();
  bool BlitToFramebuffer();

  void PrintChar(u32 ucs);
  void PrintChar(u32 ucs, u8 style);
  bool PrintNewLine(void);
  void PrintString(const char *string);
  void PrintString(const char *string, u8 style);
  void PrintSplash(u16 *screen);

private:
  FT_Library library;
  FT_Error error;

  //! Use the FreeType cache?
  bool ftc;

  // A: Homemade cache
  std::map<FT_Face, Cache *> textCache;

  // B: FreeType cache subsystem
  TextCache cache;
  TextFaceRec face_id;
  FTC_SBitRec sbit;
  FTC_ImageTypeRec imagetype;
  FT_Int charmap_index;

  std::map<u8, FT_Face> faces;
  std::map<u8, std::string> filenames;

  //! Current style, as in TEXT_STYLE_*.
  int style;
  //! Current draw position.
  FT_Vector pen;
  //! Color mode: 0=normal, 1=invert, 2=sepia
  int colorMode;
  //! Last printed char code.
  u32 codeprev;
  //! Was the last glyph lookup a cache hit?
  bool hit;

  //! It would fully justify, if it worked.
  bool justify;

  // Keep stats to check efficiency of caching.

  //! Total glyph cache hits.
  int stats_hits;
  //! Total glyph cache misses.
  int stats_misses;

  inline int CacheGlyph(u32 ucs) { return CacheGlyph(ucs, style); }
  inline int CacheGlyph(u32 ucs, u8 style) {
    return CacheGlyph(ucs, GetFace(style));
  }
  int CacheGlyph(u32 ucs, FT_Face face);
  void ClearCache(FT_Face face);
  FT_Error CreateFace(int style);
  inline FT_GlyphSlot GetGlyph(u32 ucs, int flags) {
    return GetGlyph(ucs, flags, GetFace(style));
  };
  inline FT_GlyphSlot GetGlyph(u32 ucs, int flags, u8 astyle) {
    return GetGlyph(ucs, flags, GetFace(astyle));
  };
  FT_GlyphSlot GetGlyph(u32 ucs, int flags, FT_Face face);
  FT_Error GetGlyphBitmap(u32 ucs, FTC_SBit *asbit, FTC_Node *anode = NULL);
  FT_UInt GetGlyphIndex(u32 ucs);
  u8 GetAdvance(u32 ucs, FT_Face face);
  u8 GetStringWidth(const char *txt, FT_Face face);

  int InitCache();
  FT_Error InitFreeTypeCache();

  void PrintChar(u32 ucs, FT_Face face);
  void PrintString(const char *string, FT_Face face);
  void ReportFace(FT_Face face);
};
