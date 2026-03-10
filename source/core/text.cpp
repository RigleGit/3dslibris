/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2008 Ray Haleblian

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

/*
  3DS port modifications by Rigle (summary):
  - Added dual-screen framebuffer management for 400x240 (top) and 320x240
    (bottom) software pages.
  - Added splash/background rendering helpers and sepia/gradient background mode.
  - Hardened UTF-8 handling and clipping paths used by UI labels and book text.
*/

#include "text.h"

#include <algorithm>
#include <iostream>
#include <math.h>
#include <sstream>
#include <stdio.h>
#include <sys/param.h>
#include <vector>

#include "3ds.h"
#include "string.h"

#include "app.h"
#include "main.h"
#include "stb_image.h"
#include "version.h"

#define PIXELSIZE 12

extern char msg[];
std::stringstream ss;

namespace {

static inline u16 RGB565FromU8(float r, float g, float b) {
  if (r < 0.0f)
    r = 0.0f;
  else if (r > 255.0f)
    r = 255.0f;
  if (g < 0.0f)
    g = 0.0f;
  else if (g > 255.0f)
    g = 255.0f;
  if (b < 0.0f)
    b = 0.0f;
  else if (b > 255.0f)
    b = 255.0f;

  const u16 rr = (u16)(((int)r) >> 3);
  const u16 gg = (u16)(((int)g) >> 2);
  const u16 bb = (u16)(((int)b) >> 3);
  return (u16)((rr << 11) | (gg << 5) | bb);
}

static inline void RGB565ToU8(u16 c, int *r, int *g, int *b) {
  const int r5 = (c >> 11) & 0x1F;
  const int g6 = (c >> 5) & 0x3F;
  const int b5 = c & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

static inline u16 BlendRGB565(u16 fg, u16 bg, u8 alpha) {
  int fr, fgc, fb, br, bgc, bb;
  RGB565ToU8(fg, &fr, &fgc, &fb);
  RGB565ToU8(bg, &br, &bgc, &bb);
  const int a = (int)alpha;
  const int ia = 255 - a;
  const int r = (fr * a + br * ia + 127) / 255;
  const int g = (fgc * a + bgc * ia + 127) / 255;
  const int b = (fb * a + bb * ia + 127) / 255;
  return RGB565FromU8((float)r, (float)g, (float)b);
}

static inline u16 SepiaGradientPixel(int x, int y, int w, int h) {
  static const u8 kBayer4x4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };

  const float tY = (h > 1) ? ((float)y / (float)(h - 1)) : 0.0f;
  const float dx = (w > 1)
                       ? (((float)x - (float)(w - 1) * 0.5f) /
                          ((float)(w - 1) * 0.5f))
                       : 0.0f;
  const float edge = fabsf(dx);

  float r = 244.0f + (238.0f - 244.0f) * tY;
  float g = 226.0f + (220.0f - 226.0f) * tY;
  float b = 195.0f + (185.0f - 195.0f) * tY;

  const float vignette = 1.0f - 0.12f * powf(edge, 1.8f);

  const float bayer =
      (((float)kBayer4x4[y & 3][x & 3] + 0.5f) / 16.0f) - 0.5f;
  const u32 h0 = (u32)x * 73856093u;
  const u32 h1 = (u32)y * 19349663u;
  const u32 h2 = (h0 ^ h1 ^ 0x9E3779B9u);
  const float noise = ((((h2 >> 8) & 0xFF) / 255.0f) - 0.5f) * 0.6f;

  r = r * vignette + bayer * 3.8f + noise;
  g = g * vignette + bayer * 1.9f + noise * 0.6f;
  b = b * vignette + bayer * 3.8f + noise;
  return RGB565FromU8(r, g, b);
}

static const u16 kSepiaTextColor = RGB565FromU8(70.0f, 52.0f, 32.0f);
static const u16 kSepiaBgMidColor = RGB565FromU8(241.0f, 223.0f, 190.0f);

static void FillSepiaGradient(u16 *dst, int stride, int w, int logical_h) {
  if (!dst || stride <= 0 || w <= 0 || logical_h <= 0)
    return;

  static std::vector<u16> grad320;
  static std::vector<u16> grad400;
  static int grad320w = 0;
  static int grad400w = 0;

  std::vector<u16> *grad = nullptr;
  int *cached_w = nullptr;
  const int h = (logical_h <= 320) ? 320 : 400;
  if (h == 320) {
    grad = &grad320;
    cached_w = &grad320w;
  } else {
    grad = &grad400;
    cached_w = &grad400w;
  }

  if (grad->empty() || *cached_w != w) {
    grad->resize((size_t)w * (size_t)h);
    *cached_w = w;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        (*grad)[(size_t)y * (size_t)w + (size_t)x] =
            SepiaGradientPixel(x, y, w, h);
      }
    }
  }

  const int max_y = std::min(logical_h, h);
  for (int y = 0; y < max_y; y++) {
    memcpy(dst + (size_t)y * (size_t)stride,
           grad->data() + (size_t)y * (size_t)w, (size_t)w * sizeof(u16));
  }
}

} // namespace

void Text::CopyScreen(u16 *src, u16 *dst) {
  memcpy(src, dst, display.width * display.height * sizeof(u16));
}

Text::Text() {
  display.height = PAGE_HEIGHT;
  display.width = PAGE_WIDTH;
  filenames[TEXT_STYLE_REGULAR] = FONTREGULARFILE;
  filenames[TEXT_STYLE_BOLD] = FONTBOLDFILE;
  filenames[TEXT_STYLE_ITALIC] = FONTITALICFILE;
  filenames[TEXT_STYLE_BOLDITALIC] = FONTBOLDITALICFILE;
  filenames[TEXT_STYLE_BROWSER] = FONTBROWSERFILE;

  // Allocate square buffers like DS VRAM (256x256).
  // The indexing screen[sy * display.height + sx] requires
  // PAGE_HEIGHT * PAGE_HEIGHT entries to avoid overflow.
  int bufsize = PAGE_HEIGHT * PAGE_HEIGHT;
  screenleft = new u16[bufsize];
  screenright = new u16[bufsize];
  offscreen = new u16[bufsize];
  memset(screenleft, 0xFF, bufsize * sizeof(u16));
  memset(screenright, 0xFF, bufsize * sizeof(u16));
  memset(offscreen, 0xFF, bufsize * sizeof(u16));
  margin.left = MARGINLEFT;
  margin.right = MARGINRIGHT;
  margin.top = MARGINTOP;
  margin.bottom = MARGINBOTTOM;
  bgcolor.r = 15;
  bgcolor.g = 15;
  bgcolor.b = 15;
  usebgcolor = false;
  colorMode = 0;
  justify = false;
  linespacing = 1;
  ftc = false;

  // Rendering state.
  hit = false;
  linebegan = false;
  codeprev = 0;
  bold = false;
  italic = false;
  style = TEXT_STYLE_REGULAR;
  pixelsize = PIXELSIZE;
  screen = screenleft;
  screenleft_dirty = true;
  screenright_dirty = true;

  imagetype.face_id = (FTC_FaceID)&face_id;
  imagetype.flags = FT_LOAD_DEFAULT;
  imagetype.height = pixelsize;
  imagetype.width = 0;

  // Statistics.
  stats_hits = 0;
  stats_misses = 0;
  splash_attempted = false;
  splash_loaded = false;
  splash_pixels = nullptr;

  ClearScreen(offscreen, 255, 255, 255);
  ss.clear();
}

Text::~Text() {
  // framebuffers
  delete[] offscreen;
  delete[] screenleft;
  delete[] screenright;
  delete[] splash_pixels;

  // homemade cache
  ClearCache();
  for (std::map<FT_Face, Cache *>::iterator iter = textCache.begin();
       iter != textCache.end(); iter++) {
    delete iter->second;
  }
  textCache.clear();

  // FreeType
  for (std::map<u8, FT_Face>::iterator iter = faces.begin();
       iter != faces.end(); iter++) {
    FT_Done_Face(iter->second);
  }
  FT_Done_FreeType(library);
}

static FT_Error TextFaceRequester(FTC_FaceID face_id, FT_Library library,
                                  FT_Pointer request_data, FT_Face *aface) {
  TextFace face = (TextFace)face_id; // simple typecase
  return FT_New_Face(library, face->file_path, face->face_index, aface);
}

FT_Error Text::InitFreeTypeCache(void) {
  //! Use FreeType's cache manager. borken!

  auto error = FT_Init_FreeType(&library);
  if (error)
    return error;
  error = FTC_Manager_New(library, 0, 0, 0, &TextFaceRequester, NULL,
                          &cache.manager);
  if (error)
    return error;
  error = FTC_ImageCache_New(cache.manager, &cache.image);
  if (error)
    return error;
  error = FTC_SBitCache_New(cache.manager, &cache.sbit);
  if (error)
    return error;
  error = FTC_CMapCache_New(cache.manager, &cache.cmap);
  if (error)
    return error;

  sprintf(face_id.file_path, "%s/%s", app->fontdir.c_str(),
          filenames[TEXT_STYLE_REGULAR].c_str());
  face_id.face_index = 0;
  error = FTC_Manager_LookupFace(cache.manager, (FTC_FaceID)&face_id,
                                 &faces[TEXT_STYLE_REGULAR]);
  if (error)
    return error;

  return 0;
}

FT_Error Text::CreateFace(int style) {
  std::string path = app->fontdir + "/" + filenames[style];
  error = FT_New_Face(library, path.c_str(), 0, &faces[style]);
  if (error) {
    printf("[FAIL] Font: %s\n", path.c_str());
    return error;
  }

  error = FT_Select_Charmap(faces[style], FT_ENCODING_UNICODE);
  if (error) {
    printf("[FAIL] Charmap: %s\n", path.c_str());
    return error;
  }

  auto size = pixelsize;
  if (style == TEXT_STYLE_BROWSER)
    size = 12;

  error = FT_Set_Pixel_Sizes(faces[style], 0, size);
  if (error) {
    printf("[FAIL] Pixel size\n");
    return error;
  }

  textCache.insert(std::make_pair(faces[style], new Cache()));
  printf("[OK] Font: %s\n", filenames[style].c_str());

  return error;
}

int Text::InitCache(void) {
  //! Use our own cheesey glyph cache.

  FT_Init_FreeType(&library);
  error |= CreateFace(TEXT_STYLE_BROWSER);
  error |= CreateFace(TEXT_STYLE_REGULAR);
  error |= CreateFace(TEXT_STYLE_ITALIC);
  error |= CreateFace(TEXT_STYLE_BOLD);
  error |= CreateFace(TEXT_STYLE_BOLDITALIC);

  return error;
}

int Text::Init() {
  if (ftc)
    return InitFreeTypeCache();
  else
    return InitCache();
}

void Text::ReportFace(FT_Face face) {
  printf("%s\n", face->family_name);
  printf("%s\n", face->style_name);
  printf("faces %ld\n", face->num_faces);
  printf("glyphs %ld\n", face->num_glyphs);
  printf("fixed-sizes %d\n", face->num_fixed_sizes);
  for (int i = 0; i < face->num_fixed_sizes; i++) {
    printf(" w %d h %d\n", face->available_sizes[i].width,
           face->available_sizes[i].height);
  }
}

void Text::Begin() {
  bold = false;
  italic = false;
  linebegan = false;
  stats_hits = 0;
  stats_misses = 0;
  hit = false;
}

void Text::End() {}

int Text::CacheGlyph(u32 ucs, FT_Face face) {
  //! Cache glyph at ucs if there's space.

  //! Does not check if this is a duplicate entry;
  //! The caller should have checked first.

  if (textCache[face]->cacheMap.size() == CACHESIZE)
    return -1;

  FT_Load_Char(face, ucs, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
  FT_GlyphSlot src = face->glyph;
  FT_GlyphSlot dst = new FT_GlyphSlotRec;
  int x = src->bitmap.rows;
  int y = src->bitmap.width;
  dst->bitmap.buffer = new unsigned char[x * y];
  memcpy(dst->bitmap.buffer, src->bitmap.buffer, x * y);
  dst->bitmap.rows = src->bitmap.rows;
  dst->bitmap.width = src->bitmap.width;
  dst->bitmap_top = src->bitmap_top;
  dst->bitmap_left = src->bitmap_left;
  dst->advance = src->advance;
  textCache[face]->cacheMap.insert(std::make_pair(ucs, dst));
  return ucs;
}

FT_UInt Text::GetGlyphIndex(u32 ucs) {
  //! Given a UCS codepoint, return where it is in the charmap, by index.
  if (ftc)
    return FTC_CMapCache_Lookup(cache.cmap, (FTC_FaceID)&face_id, -1, ucs);
  else
    return FT_Get_Char_Index(faces[style], ucs);
}

int Text::GetGlyphBitmap(u32 ucs, FTC_SBit *sbit, FTC_Node *anode) {
  //! Given a UCS code, fills sbit and anode.
  //! Returns nonzero on error.
  if (!ftc)
    return 1;
  imagetype.face_id = (FTC_FaceID)&face_id;
  imagetype.height = pixelsize;
  imagetype.flags = FT_LOAD_RENDER;
  return FTC_SBitCache_Lookup(cache.sbit, &imagetype, GetGlyphIndex(ucs), sbit,
                              anode);
}

FT_GlyphSlot Text::GetGlyph(u32 ucs, int flags, FT_Face face) {
  if (ftc)
    halt("error: GetGlyph() called with ftc enabled");

  std::map<u16, FT_GlyphSlot>::iterator iter =
      textCache[face]->cacheMap.find(ucs);
  if (iter != textCache[face]->cacheMap.end()) {
    stats_hits++;
    hit = true;
    return iter->second;
  }

  // No cache hit, so load glyph.
  hit = false;
  stats_misses++;
  int i = CacheGlyph(ucs, face);
  if (i >= 0)
    return textCache[face]->cacheMap[ucs];

  // Cache is full, look up directly.
  FT_Load_Char(face, ucs, flags);
  return face->glyph;
}

void Text::ClearCache() {
  for (std::map<u8, FT_Face>::iterator iter = faces.begin();
       iter != faces.end(); iter++) {
    ClearCache(iter->second);
  }
}

void Text::ClearCache(u8 style) { ClearCache(GetFace(style)); }

void Text::ClearCache(FT_Face face) {
  for (std::map<u16, FT_GlyphSlot>::iterator iter =
           textCache[face]->cacheMap.begin();
       iter != textCache[face]->cacheMap.end(); iter++) {
    delete[] iter->second->bitmap.buffer;
    delete iter->second;
  }
  textCache[face]->cacheMap.clear();
}

void Text::ClearScreen() {
  MarkCurrentScreenDirty();
  // Buffer is PAGE_HEIGHT * PAGE_HEIGHT entries
  const int bufsize = PAGE_HEIGHT * PAGE_HEIGHT;
  const int logicalHeight = (screen == screenleft) ? 400 : 320;
  u16 bg_color;
  if (colorMode == 1)
    bg_color = 0x0000;
  else
    bg_color = 0xFFFF; // Normal background (white)

  if (colorMode == 2) {
    FillSepiaGradient(screen, display.height, display.width, logicalHeight);
    return;
  }

  for (int i = 0; i < bufsize; i++) {
    screen[i] = bg_color;
  }
}

void Text::ClearRect(u16 xl, u16 yl, u16 xh, u16 yh) {
  MarkCurrentScreenDirty();
  u16 clearcolor;
  if (colorMode == 1)
    clearcolor = 0x0000;
  else
    clearcolor = 0xFFFF;
  int maxHeight = (screen == screenleft ? 400 : 320);
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < maxHeight && x < (u16)display.width) {
        if (colorMode == 2) {
          screen[y * display.height + x] =
              SepiaGradientPixel((int)x, (int)y, (int)display.width, maxHeight);
        } else {
          screen[y * display.height + x] = clearcolor;
        }
      }
    }
  }
}

u16 Text::GetFgColor() {
  if (colorMode == 0)
    return 0x0000;
  if (colorMode == 1)
    return 0xFFFF;
  return kSepiaTextColor;
}

u16 Text::GetBgColor() {
  if (colorMode == 0)
    return 0xFFFF;
  if (colorMode == 1)
    return 0x0000;
  return kSepiaBgMidColor;
}

void Text::FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirty();
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < (u16)display.height && x < (u16)display.width)
        screen[y * display.height + x] = color;
    }
  }
}

void Text::DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirty();
  int maxHeight = (screen == screenleft ? 400 : 320);
  for (u16 x = xl; x < xh; x++) {
    if (yl < maxHeight && x < (u16)display.width)
      screen[yl * display.height + x] = color;
    if (yh - 1 < maxHeight && x < (u16)display.width)
      screen[(yh - 1) * display.height + x] = color;
  }
  for (u16 y = yl; y < yh; y++) {
    if (y < maxHeight && xl < (u16)display.width)
      screen[y * display.height + xl] = color;
    if (y < maxHeight && xh - 1 < (u16)display.width)
      screen[y * display.height + xh - 1] = color;
  }
}

u8 Text::GetStringWidth(const char *txt, u8 style) {
  return GetStringWidth(txt, GetFace(style));
}

u8 Text::GetStringWidth(const char *txt, FT_Face face) {
  //! Return total advance in pixels.
  if (!txt)
    return 0;

  int width = 0;
  const char *c = txt;
  while (*c) {
    u32 ucs = 0;
    u8 bytes = GetCharCode(c, &ucs);
    if (bytes == 0) {
      // Fallback: advance one byte to avoid getting stuck on malformed UTF-8.
      c++;
      continue;
    }
    width += GetAdvance(ucs, face);
    c += bytes;
  }

  if (width < 0)
    return 0;
  if (width > 255)
    return 255;
  return (u8)width;
}

u8 Text::GetCharCountInsideWidth(const char *txt, u8 style, u8 pixels) {
  u8 n = 0;
  u32 ucs = 0;
  u8 width = 0;
  for (const char *c = txt; *c != 0 && n <= strlen(txt);) {
    u8 bytes = GetCharCode(c, &ucs);
    if (!bytes) {
      bytes = 1;
      ucs = '?';
    }
    c += bytes;
    width += GetAdvance(ucs, faces[style]);
    if (width > pixels)
      return n;
    n++;
  }
  return n;
}

u8 Text::GetCharCode(const char *utf8, u32 *ucs) {
  //! Given a UTF-8 encoding, fill in the Unicode/UCS code point.
  //! Return the bytelength of the encoding, for advancing
  //! to the next character; 0 if encoding could not be translated.
  if (!utf8 || !ucs || !utf8[0])
    return 0;

  auto cp1252ToUcs = [](unsigned char b) -> u32 {
    static const u16 cp1252_map[32] = {
        0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
        0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
    };
    if (b >= 0x80 && b <= 0x9F) {
      u16 mapped = cp1252_map[b - 0x80];
      return mapped ? mapped : (u32)'?';
    }
    return (u32)b; // Latin-1 range (0xA0..0xFF) or ASCII.
  };

  const unsigned char c0 = (unsigned char)utf8[0];
  if (c0 < 0x80) { // ASCII
    *ucs = c0;
    return 1;
  }

  if (c0 >= 0xC2 && c0 <= 0xDF) { // 2-byte UTF-8
    const unsigned char c1 = (unsigned char)utf8[1];
    if (!utf8[1] || (c1 & 0xC0) != 0x80) {
      *ucs = cp1252ToUcs(c0);
      return 1;
    }
    *ucs = ((u32)(c0 & 0x1F) << 6) | (u32)(c1 & 0x3F);
    return 2;
  }

  if (c0 >= 0xE0 && c0 <= 0xEF) { // 3-byte UTF-8
    const unsigned char c1 = (unsigned char)utf8[1];
    const unsigned char c2 = (unsigned char)utf8[2];
    if (!utf8[1] || !utf8[2] || (c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
      *ucs = cp1252ToUcs(c0);
      return 1;
    }
    *ucs = ((u32)(c0 & 0x0F) << 12) | ((u32)(c1 & 0x3F) << 6) |
           (u32)(c2 & 0x3F);
    return 3;
  }

  if (c0 >= 0xF0 && c0 <= 0xF4) { // 4-byte UTF-8
    const unsigned char c1 = (unsigned char)utf8[1];
    const unsigned char c2 = (unsigned char)utf8[2];
    const unsigned char c3 = (unsigned char)utf8[3];
    if (!utf8[1] || !utf8[2] || !utf8[3] || (c1 & 0xC0) != 0x80 ||
        (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      *ucs = cp1252ToUcs(c0);
      return 1;
    }
    *ucs = ((u32)(c0 & 0x07) << 18) | ((u32)(c1 & 0x3F) << 12) |
           ((u32)(c2 & 0x3F) << 6) | (u32)(c3 & 0x3F);
    return 4;
  }

  *ucs = cp1252ToUcs(c0);
  return 1;
}

std::string Text::GetFontName(u8 style) {
  return std::string(faces[style]->family_name) + " " +
         std::string(faces[style]->style_name);
}

u8 Text::GetHeight() { return (GetFace(style)->size->metrics.height >> 6); }

void Text::GetPen(u16 *x, u16 *y) {
  *x = pen.x;
  *y = pen.y;
}

void Text::SetPen(u16 x, u16 y) {
  pen.x = x;
  pen.y = y;
}

void Text::GetPen(u16 &x, u16 &y) {
  x = pen.x;
  y = pen.y;
}

void Text::SetColorMode(int state) { colorMode = state; }

int Text::GetColorMode() { return colorMode; }

u16 Text::GetPenX() { return pen.x; }

u16 Text::GetPenY() { return pen.y; }

u8 Text::GetPixelSize() { return pixelsize; }

u16 *Text::GetScreen() { return screen; }

void Text::SetPixelSize(u8 size) {
  if (size == pixelsize)
    return;
  pixelsize = size;
  if (ftc) {
    imagetype.height = pixelsize;
    imagetype.width = pixelsize;
  } else {
    for (auto &it : faces) {
      // UI font stays at 12.
      if (it.first == TEXT_STYLE_BROWSER)
        continue;
      FT_Set_Pixel_Sizes(it.second, 0, pixelsize);
      ClearCache(it.second);
    }
  }
}

void Text::SetScreen(u16 *inscreen) { screen = inscreen; }

u8 Text::GetAdvance(u32 ucs, FT_Face face) {
  //! Return glyph advance in pixels.

  if (ftc) {
    auto gindex = GetGlyphIndex(ucs);

    FTC_SBit sbit;
    error = FTC_SBitCache_Lookup(cache.sbit, &imagetype, gindex, &sbit, NULL);
    if (!error)
      return sbit->xadvance;

    FT_Glyph glyph;
    error =
        FTC_ImageCache_Lookup(cache.image, &imagetype, gindex, &glyph, NULL);
    if (!error)
      return (glyph->advance).x;
  } else {
#ifdef ADVANCE_NO_CACHE
    // Much slower, maybe less buggy.
    auto gindex = FT_Get_Char_Index(face, ucs);
    error = FT_Load_Glyph(face, gindex, FT_LOAD_DEFAULT);
    if (!error)
      return face->glyph->advance.x >> 6;
#else
    return GetGlyph(ucs, FT_LOAD_DEFAULT, face)->advance.x >> 6;
#endif
  }
  return 0;
}

int Text::GetStringAdvance(const char *s) {
  int advance = 0;
  for (unsigned int i = 0; i < strlen(s); i++) {
    advance += GetAdvance(s[i]);
  }
  return advance;
}

bool Text::GetFontName(std::string &s) {
  const char *name = FT_Get_Postscript_Name(GetFace(style));
  if (!name)
    return false;
  else {
    s = name;
    return true;
  }
}

void Text::InitPen(void) {
  pen.x = margin.left;
  pen.y = margin.top + GetHeight();
}

void Text::PrintChar(u32 ucs) { PrintChar(ucs, GetFace(style)); }

void Text::PrintChar(u32 ucs, u8 astyle) { PrintChar(ucs, GetFace(astyle)); }

void Text::PrintChar(u32 ucs, FT_Face face) {
  // Draw a character for the given UCS codepoint,
  // into the current screen buffer at the current pen position.
  MarkCurrentScreenDirty();

  u16 bx, by, width, height = 0;
  FT_Byte *buffer = NULL;
  FT_UInt advance = 0;
  FTC_Node anode = nullptr;
  FT_Glyph glyph;

  if (ftc) {
    auto glyph_index =
        FTC_CMapCache_Lookup(cache.cmap, (FTC_FaceID)&face_id, -1, ucs);
    // TODO set imagetype here

    FTC_SBit p = &sbit;
    error =
        FTC_SBitCache_Lookup(cache.sbit, &imagetype, glyph_index, &p, &anode);
    if (error)
      return;

    // there will typically be no bitmap, only an outline
    if (!p) {
      error = FTC_ImageCache_Lookup(cache.image, &imagetype, glyph_index,
                                    &glyph, &anode);
      if (error)
        return;
    }
    // TODO rasterize bitmap

    buffer = sbit.buffer;
    bx = sbit.left;
    by = sbit.top;
    height = sbit.height;
    width = sbit.width;
    advance = sbit.xadvance;
  } else {
    // Consult the cache for glyph data and cache it on a miss, if space is
    // available.
    FT_GlyphSlot glyph =
        GetGlyph(ucs, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL, face);

    // extract glyph image
    FT_Bitmap bitmap = glyph->bitmap;
    bx = glyph->bitmap_left;
    by = glyph->bitmap_top;
    width = bitmap.width;
    height = bitmap.rows;
    advance = glyph->advance.x >> 6;
    buffer = bitmap.buffer;
  }

#ifdef EXPERIMENTAL_KERNING
  // Fetch a kerning vector.
  if (codeprev) {
    FT_Vector kerning_vector;
    error = FT_Get_Kerning(face, codeprev, ucs, FT_KERNING_DEFAULT,
                           &kerning_vector);
  }
#endif

  // Render to framebuffer.

  // Hard clip: don't render body text below the safe area.
  // UI text (browser/status) is allowed to use the full screen height.
  int maxY = (screen == screenleft) ? 400 : 320;
  int bottomClip = margin.bottom;
  if (faces.count(TEXT_STYLE_BROWSER) &&
      face == faces[TEXT_STYLE_BROWSER]) {
    bottomClip = 0;
  }
  if (pen.y > maxY - bottomClip) {
    pen.x += advance;
    codeprev = ucs;
    if (ftc && anode)
      FTC_Node_Unref(anode, cache.manager);
    return;
  }

#ifdef DRAW_PEN_POSITION
  // Mark the pen position.
  screen[pen.y * display.height + pen.x] = RGB15(0, 0, 0) | BIT(15);
#endif

  u16 gx, gy;
  for (gy = 0; gy < height; gy++) {
    for (gx = 0; gx < width; gx++) {
      u8 a = buffer[gy * width + gx];
      if (!a)
        continue;
      // `a` is the grayscale value from freetype
      // Render colors depending on colorMode:
      // Mode 0 (Normal): bg = white (255), fg = black (0). v = 255 - a
      // Mode 1 (Dark): bg = black (0), fg = white (255). v = a
      // Mode 2 (Sepia): bg = 0xD6B4 (Cream: R=26, G=53, B=20)
      //                 fg = 0x38C0 (DarkBrown: R=7, G=6, B=0)
      u16 sx = (pen.x + gx + bx);
      u16 sy = (pen.y + gy - by);
      // Bounds check to prevent buffer overrun
      if (sy >= (u16)maxY || sx >= display.width)
        continue;
      const size_t dst_index =
          (size_t)sy * (size_t)display.height + (size_t)sx;

      u16 pixel;
      if (colorMode == 0) {
        // Blend black text over current background for correct AA on buttons.
        pixel = BlendRGB565(0x0000, screen[dst_index], a);
      } else if (colorMode == 1) {
        // Blend white text over current background in dark mode.
        pixel = BlendRGB565(0xFFFF, screen[dst_index], a);
      } else {
        pixel = BlendRGB565(kSepiaTextColor, screen[dst_index], a);
      }
#ifdef DRAW_CACHE_MISSES
      // if(!hit) pixel = RGB15(a>>3,0,0) | BIT(15);
#endif
      screen[dst_index] = pixel;
    }
  }

  pen.x += advance;
  codeprev = ucs;

  if (ftc && anode)
    FTC_Node_Unref(anode, cache.manager);
}

bool Text::PrintNewLine(void) {
  //! Render a newline at the current position.
  pen.x = margin.left;
  int height = GetHeight();
  // Use actual screen height: 400 for left (top), 320 for right (bottom)
  int maxHeight = (screen == screenleft) ? 400 : 320;
  int y = pen.y + height + linespacing;
  if (y > (maxHeight - margin.bottom)) {
    // UI strings should not spill over to the other screen.
    if (style == TEXT_STYLE_BROWSER)
      return false;
    if (screen == screenleft) {
      screen = screenright;
      pen.y = margin.top + height;
      return true;
    } else
      return false;
  } else {
    pen.y += height + linespacing;
    return true;
  }
}

void Text::PrintString(const char *s) {
  //! Render a character string starting at the pen position.
  PrintString(s, style);
}

void Text::PrintString(const char *s, u8 style) {
  //! Render a character string starting at the pen position.
  PrintString(s, GetFace(style));
}

void Text::PrintString(const char *s, FT_Face face) {
  //! Render a character string starting at the pen position.
  if (!s)
    return;
  size_t i = 0;
  size_t len = strlen((char *)s);
  while (i < len) {
    const unsigned char b = (unsigned char)s[i];
    if (b == '\n') {
      PrintNewLine();
      i++;
    } else {
      u32 c = 0;
      u8 bytes = GetCharCode(&(s[i]), &c);
      if (!bytes) {
        bytes = 1;
        c = '?';
      }
      i += bytes;
      PrintChar(c, face);
    }
  }
}

void Text::ClearScreen(u16 *screen, u8 r, u8 g, u8 b) {
  MarkScreenDirty(screen);
  u16 pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  for (int i = 0; i < display.width * display.height; i++)
    screen[i] = pixel;
}

bool Text::EnsureSplashLoaded() {
  if (splash_attempted)
    return splash_loaded;
  splash_attempted = true;

  static const char *kSplashCandidates[] = {
      "sdmc:/3ds/3dslibris/resources/splash.jpg",
      "sdmc:/3ds/3dslibris/resources/splash.jpeg",
      "sdmc:/3ds/3dslibris/splash.jpg",
      "sdmc:/3ds/3dslibris/splash.jpeg",
      nullptr,
  };

  int srcW = 0;
  int srcH = 0;
  int srcChannels = 0;
  unsigned char *srcRgb = nullptr;
  for (int i = 0; kSplashCandidates[i] != nullptr; i++) {
    FILE *fp = fopen(kSplashCandidates[i], "rb");
    if (!fp)
      continue;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
      fclose(fp);
      continue;
    }
    std::vector<unsigned char> encoded((size_t)size);
    if (fread(encoded.data(), 1, (size_t)size, fp) != (size_t)size) {
      fclose(fp);
      continue;
    }
    fclose(fp);
    srcRgb = stbi_load_from_memory(encoded.data(), (int)encoded.size(), &srcW,
                                   &srcH, &srcChannels, 3);
    if (srcRgb)
      break;
  }
  if (!srcRgb || srcW <= 0 || srcH <= 0)
    return false;

  const int targetW = display.width;
  const int targetH = display.height;
  splash_pixels = new u16[(size_t)targetW * (size_t)targetH];
  if (!splash_pixels) {
    stbi_image_free(srcRgb);
    return false;
  }

  int scaledW = 0;
  int scaledH = 0;
  int cropX = 0;
  int cropY = 0;

  if ((long long)srcW * (long long)targetH >
      (long long)srcH * (long long)targetW) {
    // Wider than target: fit by height and crop horizontal overflow.
    scaledH = targetH;
    scaledW = (int)(((long long)srcW * (long long)targetH + srcH - 1) / srcH);
    cropX = (scaledW - targetW) / 2;
  } else {
    // Taller than target: fit by width and crop vertical overflow.
    scaledW = targetW;
    scaledH = (int)(((long long)srcH * (long long)targetW + srcW - 1) / srcW);
    cropY = (scaledH - targetH) / 2;
  }
  if (scaledW <= 0 || scaledH <= 0) {
    stbi_image_free(srcRgb);
    delete[] splash_pixels;
    splash_pixels = nullptr;
    return false;
  }

  for (int y = 0; y < targetH; y++) {
    int sy = ((y + cropY) * srcH) / scaledH;
    sy = std::max(0, std::min(srcH - 1, sy));
    for (int x = 0; x < targetW; x++) {
      int sx = ((x + cropX) * srcW) / scaledW;
      sx = std::max(0, std::min(srcW - 1, sx));

      const unsigned char *p = srcRgb + ((size_t)sy * (size_t)srcW + sx) * 3;
      const u16 r = (u16)(p[0] >> 3);
      const u16 g = (u16)(p[1] >> 2);
      const u16 b = (u16)(p[2] >> 3);
      splash_pixels[(size_t)y * (size_t)targetW + (size_t)x] =
          (u16)((r << 11) | (g << 5) | b);
    }
  }

  stbi_image_free(srcRgb);
  splash_loaded = true;
  return true;
}

void Text::DrawFallbackSplash() {
  int savedStyle = GetStyle();
  int savedPixelSize = pixelsize;

  SetStyle(TEXT_STYLE_BROWSER);
  SetPixelSize(16);
  SetPen(20, 44);
  PrintString("3dslibris");
  SetPixelSize(12);
  SetPen(22, 66);
  PrintString("by Rigle");

  SetPixelSize(savedPixelSize);
  SetStyle(savedStyle);
}

void Text::PrintSplash(u16 *screen) {
  // push
  auto s = GetScreen();
  int savedColorMode = GetColorMode();

  SetScreen(screen);
  SetColorMode(0);
  ClearScreen();
  if (screen == screenleft && EnsureSplashLoaded()) {
    for (int y = 0; y < display.height; y++) {
      u16 *dst = screen + y * display.height;
      const u16 *src = splash_pixels + y * display.width;
      for (int x = 0; x < display.width; x++)
        dst[x] = src[x];
    }
  } else {
    drawstack(screen);
    if (screen == screenleft)
      DrawFallbackSplash();
  }
  MarkScreenDirty(screen);
  SetColorMode(savedColorMode);
  // pop
  SetScreen(s);
}

void Text::SetFontFile(const char *path, u8 style) {
  if (!strcmp(filenames[style].c_str(), path))
    return;
  filenames[style] = std::string(path);
  CreateFace(style);
}

std::string Text::GetFontFile(u8 style) { return filenames[style]; }

void Text::MarkScreenDirty(u16 *target) {
  if (target == screenleft)
    screenleft_dirty = true;
  else if (target == screenright)
    screenright_dirty = true;
}

bool Text::BlitToFramebuffer() {
  if (!screenleft_dirty && !screenright_dirty)
    return false;

  u16 fbW, fbH;

  auto blitPage = [&](u8 *fb, u16 *src, u16 logicalHeight) {
    if (!fb || !src)
      return;

    // libctru can report dimensions in either orientation depending on screen.
    // Use stride=min and width=max to keep mapping stable.
    const u32 stride = (fbW < fbH) ? fbW : fbH;         // usually 240
    const u32 physWidth = (fbW > fbH) ? fbW : fbH;      // 320 or 400
    const size_t fbSize = (size_t)fbW * (size_t)fbH * 3;
    if (!stride || !physWidth || !fbSize)
      return;

    memset(fb, 0xFF, fbSize);

    u32 maxSy = logicalHeight;
    if (maxSy > physWidth)
      maxSy = physWidth;
    u32 maxSx = (u32)display.width;
    if (maxSx > stride)
      maxSx = stride;

    for (u32 sy = 0; sy < maxSy; sy++) {
      for (u32 sx = 0; sx < maxSx; sx++) {
        u16 pixel = src[sy * display.height + sx];
        if (pixel == 0xFFFF)
          continue;

        u8 r = ((pixel >> 11) & 0x1F) << 3;
        u8 g = ((pixel >> 5) & 0x3F) << 2;
        u8 b = (pixel & 0x1F) << 3;

        // Rotate logical page into linear framebuffer layout.
        const u32 dx = physWidth - 1 - sy;
        const u32 dy = stride - 1 - sx;
        const size_t off = ((size_t)dx * (size_t)stride + (size_t)dy) * 3;
        if (off + 2 >= fbSize)
          continue;

        fb[off + 0] = b;
        fb[off + 1] = g;
        fb[off + 2] = r;
      }
    }
  };

  // With double-buffering on 3DS, a swap flips both screens. If we only write
  // one side, the other side can show stale content from an older backbuffer.
  // So when anything is dirty, refresh both software pages into current
  // backbuffers before swapping.
  u8 *fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);
  blitPage(fbBottom, screenright, 320);

  u8 *fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
  blitPage(fbTop, screenleft, 400);

  screenright_dirty = false;
  screenleft_dirty = false;
  return true;
}
