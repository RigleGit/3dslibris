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

#include "text.h"

#include <iostream>
#include <sstream>
#include <sys/param.h>

#include "3ds.h"
#include "string.h"

#include "app.h"
#include "main.h"
#include "version.h"

#define PIXELSIZE 12

extern char msg[];
std::stringstream ss;

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

  imagetype.face_id = (FTC_FaceID)&face_id;
  imagetype.flags = FT_LOAD_DEFAULT;
  imagetype.height = pixelsize;
  imagetype.width = 0;

  // Statistics.
  stats_hits = 0;
  stats_misses = 0;

  ClearScreen(offscreen, 255, 255, 255);
  ss.clear();
}

Text::~Text() {
  // framebuffers
  delete[] offscreen;
  delete[] screenleft;
  delete[] screenright;

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
  // Buffer is PAGE_HEIGHT * PAGE_HEIGHT entries
  const int bufsize = PAGE_HEIGHT * PAGE_HEIGHT;
  u16 bg_color;
  if (colorMode == 1)
    bg_color = 0x0000;
  else if (colorMode == 2)
    bg_color = 0xD6B4; // Sepia background (cream-ish)
  else
    bg_color = 0xFFFF; // Normal background (white)

  for (int i = 0; i < bufsize; i++) {
    screen[i] = bg_color;
  }
}

void Text::ClearRect(u16 xl, u16 yl, u16 xh, u16 yh) {
  u16 clearcolor;
  if (colorMode == 1)
    clearcolor = 0x0000;
  else if (colorMode == 2)
    clearcolor = 0xD6B4;
  else
    clearcolor = 0xFFFF;
  int maxHeight = (screen == screenleft ? 400 : 320);
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < maxHeight && x < (u16)display.width)
        screen[y * display.height + x] = clearcolor;
    }
  }
}

u16 Text::GetFgColor() {
  if (colorMode == 0)
    return 0x0000;
  if (colorMode == 1)
    return 0xFFFF;
  return 0x38C0; // Sepia foreground
}

u16 Text::GetBgColor() {
  if (colorMode == 0)
    return 0xFFFF;
  if (colorMode == 1)
    return 0x0000;
  return 0xD6B4; // Sepia background
}

void Text::FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < (u16)display.height && x < (u16)display.width)
        screen[y * display.height + x] = color;
    }
  }
}

void Text::DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
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
    c += GetCharCode(c, &ucs);
    if (ucs == 0)
      continue;
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

  if (utf8[0] < 0x80) { // ASCII
    *ucs = utf8[0];
    return 1;

  } else if (utf8[0] > 0xc1 && utf8[0] < 0xe0) { // latin
    *ucs = ((utf8[0] - 192) * 64) + (utf8[1] - 128);
    return 2;

  } else if (utf8[0] > 0xdf && utf8[0] < 0xf0) { // asian
    *ucs = (utf8[0] - 224) * 4096 + (utf8[1] - 128) * 64 + (utf8[2] - 128);
    return 3;

  } else if (utf8[0] > 0xef) {
    *ucs = ((utf8[0] & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) |
           ((utf8[2] & 0x3F) << 6) | (utf8[3] & 0x3F);
    return 4;
  }
  return 0;
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
      u16 pixel;
      if (colorMode == 0) {
        u8 v = (255 - a) >> 3;
        pixel = (v << 11) | (v << 6) | v;
      } else if (colorMode == 1) {
        u8 v = a >> 3;
        pixel = (v << 11) | (v << 6) | v;
      } else {
        // Sepia blending (RGB565).
        // Background: R=26, G=53, B=20
        // Foreground: R=7, G=6, B=0
        u8 R = ((7 * a) + (26 * (255 - a))) / 255;
        u8 G = ((6 * a) + (53 * (255 - a))) / 255;
        u8 B = ((0 * a) + (20 * (255 - a))) / 255;
        pixel = (R << 11) | (G << 5) | B; // G has 6 bits in RGB565
      }
#ifdef DRAW_CACHE_MISSES
      // if(!hit) pixel = RGB15(a>>3,0,0) | BIT(15);
#endif
      u16 sx = (pen.x + gx + bx);
      u16 sy = (pen.y + gy - by);
      // Bounds check to prevent buffer overrun
      if (sy < (u16)maxY && sx < display.width)
        screen[sy * display.height + sx] = pixel;
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
  u8 i = 0;
  while (i < strlen((char *)s)) {
    u32 c = s[i];
    if (c == '\n') {
      PrintNewLine();
      i++;
    } else {
      i += GetCharCode(&(s[i]), &c);
      PrintChar(c, face);
    }
  }
}

void Text::ClearScreen(u16 *screen, u8 r, u8 g, u8 b) {
  u16 pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  for (int i = 0; i < display.width * display.height; i++)
    screen[i] = pixel;
}

void Text::PrintSplash(u16 *screen) {
  // push
  auto s = GetScreen();

  SetScreen(screen);
  drawstack(screen);
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

void Text::BlitToFramebuffer() {
  u16 fbW, fbH;
  static int blitDiagCount = 0;

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

    if (app && blitDiagCount < 8) {
      char diag[196];
      snprintf(diag, sizeof(diag),
               "BLIT diag #%d fbW=%u fbH=%u stride=%lu physW=%lu logicalH=%u "
               "maxSy=%lu maxSx=%lu src=%p fb=%p",
               blitDiagCount, fbW, fbH, (unsigned long)stride,
               (unsigned long)physWidth, logicalHeight, (unsigned long)maxSy,
               (unsigned long)maxSx, (void *)src, (void *)fb);
      app->PrintStatus(diag);
    }

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

  // Bottom screen: right page (320x240 physical)
  u8 *fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);
  blitPage(fbBottom, screenright, 320);

  // Top screen: left page (400x240 physical)
  u8 *fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
  blitPage(fbTop, screenleft, 400);

  if (blitDiagCount < 8)
    blitDiagCount++;
}
