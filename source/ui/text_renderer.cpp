// Glyph rasterization, text flow, pen/color management, and splash rendering.
// Screen surface operations and framebuffer blit are in text_screen_surface.cpp.

#include "ui/text_renderer.h"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <vector>

#include "3ds.h"
#include "shared/bugfix_utils.h"
#include "shared/color_utils.h"
#include "shared/debug_log.h"
#include "shared/path_constants.h"
#include "shared/text_render_layout_utils.h"
#include "stb_image.h"
#include "string.h"
#include "ui/frame_debug_utils.h"
#include "ui/screen_layout_constants.h"
#include "ui/text.h"
#include "ui/text_buffer_utils.h"
#include "ui/theme_colors.h"

namespace {

static inline void RGB565ToU8(u16 c, int *r, int *g, int *b) {
  const int r5 = (c >> 11) & 0x1F;
  const int g6 = (c >> 5) & 0x3F;
  const int b5 = c & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

static inline int div255(int x) { return (x * 257 + 128) >> 16; }

static inline u16 BlendRGB565(u16 fg, u16 bg, u8 alpha) {
  int fr, fgc, fb, br, bgc, bb;
  RGB565ToU8(fg, &fr, &fgc, &fb);
  RGB565ToU8(bg, &br, &bgc, &bb);
  const int a = (int)alpha;
  const int ia = 255 - a;
  const int r = div255(fr * a + br * ia + 127);
  const int g = div255(fgc * a + bgc * ia + 127);
  const int b = div255(fb * a + bb * ia + 127);
  return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#ifdef DSLIBRIS_DEBUG
static int g_text_clip_right_budget = 32;
static int g_text_margin_diag_budget = 12;
#endif

}  // anonymous namespace

TextRenderer::TextRenderer(Text *owner)
    : parent(owner), turned_right(false), style(TEXT_STYLE_REGULAR), codeprev(0),
      hit(false), justify(false), colorMode(0), splash_light_attempted(false),
      splash_light_loaded(false), splash_light_pixels(nullptr),
      splash_dark_attempted(false), splash_dark_loaded(false),
      splash_dark_pixels(nullptr), stats_hits(0), stats_misses(0),
      auto_wrap_enabled(true), clip_to_content_enabled(false), script_scale_(1.0f) {
  pen.x = 0;
  pen.y = 0;
}

TextRenderer::~TextRenderer() {
  delete[] splash_light_pixels;
  delete[] splash_dark_pixels;
}

void TextRenderer::GetPen(u16 *x, u16 *y) {
  *x = pen.x;
  *y = pen.y;
}

void TextRenderer::SetPen(u16 x, u16 y) {
  pen.x = x;
  pen.y = y;
}

void TextRenderer::GetPen(u16 &x, u16 &y) {
  x = pen.x;
  y = pen.y;
}

void TextRenderer::SetColorMode(int state) {
  int old_dark = (colorMode == 1 || colorMode == 4 || colorMode == 5) ? 1 : 0;
  int new_dark = (state == 1 || state == 4 || state == 5) ? 1 : 0;
  if (old_dark != new_dark) {
    delete[] splash_light_pixels;
    splash_light_pixels = nullptr;
    splash_light_attempted = false;
    splash_light_loaded = false;
    delete[] splash_dark_pixels;
    splash_dark_pixels = nullptr;
    splash_dark_attempted = false;
    splash_dark_loaded = false;
  }
  colorMode = state;
}

int TextRenderer::GetColorMode() { return colorMode; }

void TextRenderer::SetOrientation(bool right) {
  if (turned_right == right)
    return;
  turned_right = right;
  MarkAllScreensDirty();
}

bool TextRenderer::GetOrientation() const { return turned_right; }

u16 TextRenderer::GetPenX() { return pen.x; }

u16 TextRenderer::GetPenY() { return pen.y; }

u16 *TextRenderer::GetScreen() { return parent->screen; }

void TextRenderer::SetScreen(u16 *inscreen) { parent->screen = inscreen; }

void TextRenderer::SetTextColorOverride(u16 color) {
  parent->fgcolor = color;
  parent->usefgcolor = true;
}

void TextRenderer::ClearTextColorOverride() { parent->usefgcolor = false; }

void TextRenderer::PrintChar(u32 ucs) { PrintChar(ucs, parent->GetFace((u8)style)); }

void TextRenderer::PrintChar(u32 ucs, u8 astyle) { PrintChar(ucs, parent->GetFace(astyle)); }

void TextRenderer::PrintChar(u32 ucs, FT_Face face) {
  int bx, by;
  u16 width, height = 0;
  FT_Byte *buffer = NULL;
  FT_UInt advance = 0;

  FT_GlyphSlot asglyph =
      parent->GetGlyph(ucs, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL, face);
  FT_Bitmap bitmap = asglyph->bitmap;
  bx = asglyph->bitmap_left;
  by = asglyph->bitmap_top;
  width = bitmap.width;
  height = bitmap.rows;
  advance = asglyph->advance.x >> 6;
  buffer = bitmap.buffer;

  int maxY = ScreenHeightPx(parent->screen, parent);
  int bottomClip = parent->margin.bottom;
  if (face == parent->GetFace(TEXT_STYLE_BROWSER))
    bottomClip = 0;
  if (!text_render_layout_utils::CurrentLineFitsScreen(
          (int)pen.y, parent->GetHeight(), parent->linespacing, maxY,
          bottomClip)) {
    pen.x += advance;
    codeprev = ucs;
    return;
  }

  // Pre-resolve foreground color components (constant for this character)
  int fg_r, fg_g, fg_b;
  const u16 fg_color = GetFgColor();
  RGB565ToU8(fg_color, &fg_r, &fg_g, &fg_b);

  const int screenWidth = (int)parent->display.width;
  const int contentRight = screenWidth - (int)parent->margin.right;
  int clipRight = text_render_layout_utils::ResolveClipRight(
      screenWidth, contentRight, clip_to_content_enabled);

  const bool is_browser_style = (face == parent->GetFace(TEXT_STYLE_BROWSER));

  if (script_scale_ != 1.0f && script_scale_ > 0.0f) {
    const float s = script_scale_;
    const int sc_bx = (int)(bx * s);
    const int sc_by = (int)(by * s);
    const int sc_w = std::max(1, (int)((int)width * s));
    const int sc_h = std::max(1, (int)((int)height * s));
    const int sc_adv = std::max(1, (int)(advance * s));

    MarkCurrentScreenDirtyRect((int)pen.x + sc_bx, (int)pen.y - sc_by,
                               (int)pen.x + sc_bx + sc_w,
                               (int)pen.y - sc_by + sc_h);

    if (width > 0) {
      const int glyph_right_sc = (int)pen.x + sc_bx + sc_w;
      if (text_render_layout_utils::ShouldAutoWrapGlyph(
              auto_wrap_enabled, is_browser_style, (int)pen.x,
              (int)parent->margin.left, glyph_right_sc, contentRight)) {
        if (PrintNewLine()) {
        } else {
          pen.x += sc_adv;
          codeprev = ucs;
          return;
        }
      }
    }

    // Box-filter downscale: each destination pixel averages all source pixels
    // whose source-space footprint overlaps it. Prevents missing strokes from
    // nearest-neighbor skipping at scale ~0.70 (inv_s ~1.43).
    // Alternative: render at scaled pixelsize via SetPixelSize(size*s) for
    // native FreeType hinting. Costs ~2 full glyph-cache flushes per
    // superscript character (SetPixelSize clears render cache each call).
    const float inv_s = 1.0f / s;
    const int src_w = (int)width;
    const int src_h = (int)height;
    for (int dy = 0; dy < sc_h; dy++) {
      const int sy0 = (int)(dy * inv_s);
      const int sy1 = std::min((int)((dy + 1) * inv_s), src_h - 1);
      for (int dx = 0; dx < sc_w; dx++) {
        const int sx0 = (int)(dx * inv_s);
        const int sx1 = std::min((int)((dx + 1) * inv_s), src_w - 1);
        // Accumulate source alpha over the box region
        int sum = 0, count = 0;
        for (int ky = sy0; ky <= sy1; ky++) {
          for (int kx = sx0; kx <= sx1; kx++) {
            sum += buffer[ky * src_w + kx];
            count++;
          }
        }
        u8 a = (u8)(sum / count);
        if (!a)
          continue;
        int sx = (int)pen.x + dx + sc_bx;
        int sy = (int)pen.y + dy - sc_by;
        if (sy < 0 || sy >= maxY - bottomClip || sx < 0 || sx >= screenWidth)
          continue;
        if (!GlyphWithinContentRight(sx, clipRight))
          continue;
        const size_t dst_index =
            (size_t)sy * (size_t)parent->display.height + (size_t)sx;
        int br, bgc, bb;
        RGB565ToU8(parent->screen[dst_index], &br, &bgc, &bb);
        const int ia = 255 - (int)a;
        const int r = div255(fg_r * (int)a + br * ia + 127);
        const int g = div255(fg_g * (int)a + bgc * ia + 127);
        const int b = div255(fg_b * (int)a + bb * ia + 127);
        parent->screen[dst_index] =
            (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
    }
    pen.x += sc_adv;
  } else {
    MarkCurrentScreenDirtyRect((int)pen.x + (int)bx, (int)pen.y - (int)by,
                               (int)pen.x + (int)bx + (int)width,
                               (int)pen.y - (int)by + (int)height);

    // For wide glyphs (CJK/RTL), line-breaking can land exactly at the right
    // boundary and overflow visually. Wrap before drawing when possible.
    if (width > 0) {
      const int glyph_right = (int)pen.x + bx + (int)width;
      if (text_render_layout_utils::ShouldAutoWrapGlyph(
              auto_wrap_enabled, is_browser_style, (int)pen.x,
              (int)parent->margin.left, glyph_right, contentRight)) {
        if (PrintNewLine()) {
          // Recompute line clip reference after wrap.
        } else {
          pen.x += advance;
          codeprev = ucs;
          return;
        }
      }
    }
#ifdef DSLIBRIS_DEBUG
    const int pen_x_before = (int)pen.x;
    const bool on_left_screen = (parent->screen == parent->screenleft);
    int clipped_right_pixels = 0;
    int layout_overflow_pixels = 0;
    if (parent->reporter_ && g_text_margin_diag_budget > 0 &&
        (contentRight <= 0 || contentRight > screenWidth ||
         contentRight < screenWidth - 32)) {
      DBG_LOGF_CAT(parent->reporter_, DBG_LEVEL_TRACE, DBG_CAT_CLIP,
                   "TXT margin side=%s sw=%d mr=%d content_right=%d pen=%d,%d style=%d",
                   on_left_screen ? "left" : "right", screenWidth,
                   (int)parent->margin.right, contentRight, pen_x_before,
                   (int)pen.y, (int)style);
      g_text_margin_diag_budget--;
    }
#endif

    for (u16 gy = 0; gy < height; gy++) {
      for (u16 gx = 0; gx < width; gx++) {
        u8 a = buffer[gy * width + gx];
        if (!a)
          continue;
        int sx = (int)pen.x + (int)gx + bx;
        int sy = (int)pen.y + (int)gy - by;
        if (sy < 0 || sy >= maxY - bottomClip || sx < 0 || sx >= screenWidth)
          continue;
#ifdef DSLIBRIS_DEBUG
        if (sx >= contentRight)
          layout_overflow_pixels++;
#endif
        if (!GlyphWithinContentRight(sx, clipRight)) {
#ifdef DSLIBRIS_DEBUG
          clipped_right_pixels++;
#endif
          continue;
        }
        const size_t dst_index =
            (size_t)sy * (size_t)parent->display.height + (size_t)sx;

        int br, bgc, bb;
        RGB565ToU8(parent->screen[dst_index], &br, &bgc, &bb);
        const int ia = 255 - (int)a;
        const int r = div255(fg_r * (int)a + br * ia + 127);
        const int g = div255(fg_g * (int)a + bgc * ia + 127);
        const int b = div255(fg_b * (int)a + bb * ia + 127);
        parent->screen[dst_index] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
    }

#ifdef DSLIBRIS_DEBUG
    if (parent->reporter_ &&
        (clipped_right_pixels > 0 || layout_overflow_pixels > 0) &&
        g_text_clip_right_budget > 0) {
      DBG_LOGF_CAT(parent->reporter_, DBG_LEVEL_TRACE, DBG_CAT_CLIP,
                   "TXT clipR side=%s ucs=%lu pen=%d bx=%d w=%u overflow=%d clip=%d layout_right=%d clip_right=%d mr=%d style=%d",
                   on_left_screen ? "left" : "right", (unsigned long)ucs,
                   pen_x_before, bx, (unsigned)width, layout_overflow_pixels,
                   clipped_right_pixels, contentRight, clipRight,
                   (int)parent->margin.right, (int)style);
      g_text_clip_right_budget--;
    }
#endif

    pen.x += advance;
  }
  codeprev = ucs;
}

bool TextRenderer::PrintNewLine() {
  pen.x = parent->margin.left;
  int height = parent->GetHeight();
  int maxHeight = ScreenHeightPx(parent->screen, parent);
  int y = pen.y + height + parent->linespacing;
  if (!text_render_layout_utils::CurrentLineFitsScreen(
          y, height, parent->linespacing, maxHeight, parent->margin.bottom)) {
    if (style == TEXT_STYLE_BROWSER)
      return false;
    if (parent->screen == parent->screenleft) {
      parent->screen = parent->screenright;
      pen.y = parent->margin.top + height;
      return true;
    }
    return false;
  }
  pen.y += height + parent->linespacing;
  return true;
}

void TextRenderer::PrintString(const char *s) { PrintString(s, (u8)style); }

void TextRenderer::PrintString(const char *s, u8 astyle) {
  PrintString(s, parent->GetFace(astyle));
}

void TextRenderer::PrintString(const char *s, FT_Face face) {
  if (!s)
    return;
  size_t i = 0;
  size_t len = strlen(s);
  while (i < len) {
    const unsigned char b = (unsigned char)s[i];
    if (b == '\n') {
      PrintNewLine();
      i++;
    } else {
      u32 c = 0;
      u8 bytes = parent->GetCharCode(s + i, len - i, &c);
      if (!bytes) {
        bytes = 1;
        c = '?';
      }
      i += bytes;
      PrintChar(c, face);
    }
  }
}

bool TextRenderer::EnsureSplashLoaded(bool dark) {
  bool &attempted = dark ? splash_dark_attempted : splash_light_attempted;
  bool &loaded = dark ? splash_dark_loaded : splash_light_loaded;
  u16 *&pixels = dark ? splash_dark_pixels : splash_light_pixels;

  if (attempted)
    return loaded;
  attempted = true;

  const std::string sdmc_resource_dir = paths::GetResourceDir();
  const std::vector<std::string> splash_paths = paths::GetSplashPathList();
  const std::vector<std::string> candidates = dark
      ? std::vector<std::string>{
            "romfs:/3ds/3dslibris/resources/3DSLibris_dark_small.jpg",
            sdmc_resource_dir + "/3DSLibris_dark_small.jpg",
        }
      : std::vector<std::string>{
            "romfs:/3ds/3dslibris/resources/3DSLibris_light_small.jpg",
            sdmc_resource_dir + "/3DSLibris_light_small.jpg",
            splash_paths[0],
            splash_paths[1],
        };

  int srcW = 0;
  int srcH = 0;
  int srcChannels = 0;
  unsigned char *srcRgb = nullptr;
  for (size_t i = 0; i < candidates.size(); i++) {
    FILE *fp = fopen(candidates[i].c_str(), "rb");
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

  const int targetW = parent->display.width;
  const int targetH = parent->display.height;
  pixels = new u16[(size_t)targetW * (size_t)targetH];
  if (!pixels) {
    stbi_image_free(srcRgb);
    return false;
  }

  int scaledW = 0;
  int scaledH = 0;
  int cropX = 0;
  int cropY = 0;

  if ((long long)srcW * (long long)targetH >
      (long long)srcH * (long long)targetW) {
    scaledH = targetH;
    scaledW = (int)(((long long)srcW * (long long)targetH + srcH - 1) / srcH);
    cropX = (scaledW - targetW) / 2;
  } else {
    scaledW = targetW;
    scaledH = (int)(((long long)srcH * (long long)targetW + srcW - 1) / srcW);
    cropY = (scaledH - targetH) / 2;
  }
  if (scaledW <= 0 || scaledH <= 0) {
    stbi_image_free(srcRgb);
    delete[] pixels;
    pixels = nullptr;
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
      pixels[(size_t)y * (size_t)targetW + (size_t)x] =
          (u16)((r << 11) | (g << 5) | b);
    }
  }

  stbi_image_free(srcRgb);
  loaded = true;
  return true;
}

void TextRenderer::DrawFallbackSplash() {
  int savedStyle = GetStyle();
  int savedPixelSize = parent->pixelsize;

  SetStyle(TEXT_STYLE_BROWSER);
  parent->SetPixelSize(16);
  SetPen(20, 44);
  PrintString("3dslibris");
  parent->SetPixelSize(12);
  SetPen(22, 66);
  PrintString("by Rigle");

  parent->SetPixelSize((u8)savedPixelSize);
  SetStyle(savedStyle);
}

void TextRenderer::PrintSplash(u16 *screen) {
  auto s = GetScreen();
  int savedColorMode = GetColorMode();

  SetScreen(screen);
  ClearScreen();
  if (screen == parent->screenleft) {
    bool dark = (colorMode == 1 || colorMode == 4 || colorMode == 5);
    if (EnsureSplashLoaded(dark)) {
      u16 *pixels = dark ? splash_dark_pixels : splash_light_pixels;
      for (int y = 0; y < parent->display.height; y++) {
        u16 *dst = screen + y * parent->display.height;
        const u16 *src = pixels + y * parent->display.width;
        for (int x = 0; x < parent->display.width; x++)
          dst[x] = src[x];
      }
    } else {
      DrawFallbackSplash();
    }
  }
  MarkScreenDirty(screen);
  SetColorMode(savedColorMode);
  SetScreen(s);
}

int TextRenderer::GetDisplayWidth() const { return parent->display.width; }

int TextRenderer::GetDisplayHeight() const { return parent->display.height; }

int TextRenderer::GetStyle() const { return style; }

void TextRenderer::SetStyle(int s) { style = s; }

int TextRenderer::GetPixelSize() const { return parent->pixelsize; }

void TextRenderer::SetPixelSizeVal(u8 s) { parent->pixelsize = s; }

bool TextRenderer::GetFtc() const { return false; }

u32 TextRenderer::GetCodeprev() const { return codeprev; }

void TextRenderer::SetCodeprev(u32 c) { codeprev = c; }

bool TextRenderer::IsHit() const { return hit; }

void TextRenderer::SetHit(bool h) { hit = h; }

bool TextRenderer::IsLinebegan() const { return parent->linebegan; }

void TextRenderer::SetLinebegan(bool b) { parent->linebegan = b; }

bool TextRenderer::IsBold() const { return parent->bold; }

void TextRenderer::SetBold(bool b) { parent->bold = b; }

bool TextRenderer::IsItalic() const { return parent->italic; }

void TextRenderer::SetItalic(bool b) { parent->italic = b; }

bool TextRenderer::IsJustify() const { return justify; }

int TextRenderer::GetLinespacing() const { return parent->linespacing; }

bool TextRenderer::IsAutoWrapEnabled() const { return auto_wrap_enabled; }

void TextRenderer::SetAutoWrapEnabled(bool enabled) {
  auto_wrap_enabled = enabled;
}

bool TextRenderer::IsClipToContentEnabled() const {
  return clip_to_content_enabled;
}

void TextRenderer::SetClipToContentEnabled(bool enabled) {
  clip_to_content_enabled = enabled;
}

void TextRenderer::SetScriptScale(float s) { script_scale_ = s; }

void TextRenderer::InitPen() {
  pen.x = parent->margin.left;
  pen.y = parent->margin.top + parent->GetHeight();
}
