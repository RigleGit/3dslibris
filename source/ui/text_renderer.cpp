#include "ui/text_renderer.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>

#include "3ds.h"
#include "app/app.h"
#include "color_utils.h"
#include "stb_image.h"
#include "string.h"
#include "ui/text.h"
#include "ui/text_buffer_utils.h"

namespace {

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
  const float dx =
      (w > 1) ? (((float)x - (float)(w - 1) * 0.5f) / ((float)(w - 1) * 0.5f))
              : 0.0f;
  const float edge = fabsf(dx);

  float r = 244.0f + (238.0f - 244.0f) * tY;
  float g = 226.0f + (220.0f - 226.0f) * tY;
  float b = 195.0f + (185.0f - 195.0f) * tY;

  const float vignette = 1.0f - 0.12f * powf(edge, 1.8f);

  const float bayer = (((float)kBayer4x4[y & 3][x & 3] + 0.5f) / 16.0f) - 0.5f;
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

}

TextRenderer::TextRenderer(Text *owner)
    : parent(owner), turned_right(false), style(TEXT_STYLE_REGULAR), codeprev(0),
      hit(false), justify(false), colorMode(0), splash_attempted(false),
      splash_loaded(false), splash_pixels(nullptr), stats_hits(0),
      stats_misses(0) {
  pen.x = 0;
  pen.y = 0;
}

TextRenderer::~TextRenderer() { delete[] splash_pixels; }

void TextRenderer::CopyScreen(u16 *src, u16 *dst) {
  memcpy(dst, src, parent->display.width * parent->display.height * sizeof(u16));
}

void TextRenderer::ClearScreen() {
  const bool is_left_screen = (parent->screen == parent->screenleft);
  const int logicalHeight =
      framebuffer_blit_utils::LogicalTextScreenHeight(is_left_screen);
  MarkCurrentScreenDirtyRect(0, 0, parent->display.width, logicalHeight);
  u16 bg_color;
  if (colorMode == 1)
    bg_color = 0x0000;
  else
    bg_color = 0xFFFF;

  if (colorMode == 2) {
    FillSepiaGradient(parent->screen, parent->display.height, parent->display.width,
                      logicalHeight);
    return;
  }

  text_buffer_utils::FillLogicalScreenRows(parent->screen, parent->display.height,
                                           parent->display.width, logicalHeight,
                                           bg_color);
}

void TextRenderer::ClearRect(u16 xl, u16 yl, u16 xh, u16 yh) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  u16 clearcolor;
  if (colorMode == 1)
    clearcolor = 0x0000;
  else
    clearcolor = 0xFFFF;
  int maxHeight = (parent->screen == parent->screenleft ? 400 : 320);
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < maxHeight && x < (u16)parent->display.width) {
        if (colorMode == 2) {
          parent->screen[y * parent->display.height + x] = SepiaGradientPixel(
              (int)x, (int)y, (int)parent->display.width, maxHeight);
        } else {
          parent->screen[y * parent->display.height + x] = clearcolor;
        }
      }
    }
  }
}

u16 TextRenderer::GetFgColor() {
  if (parent->usefgcolor)
    return parent->fgcolor;
  if (colorMode == 0)
    return 0x0000;
  if (colorMode == 1)
    return 0xFFFF;
  return kSepiaTextColor;
}

u16 TextRenderer::GetBgColor() {
  if (colorMode == 0)
    return 0xFFFF;
  if (colorMode == 1)
    return 0x0000;
  return kSepiaBgMidColor;
}

void TextRenderer::FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  for (u16 y = yl; y < yh; y++) {
    for (u16 x = xl; x < xh; x++) {
      if (y < (u16)parent->display.height && x < (u16)parent->display.width)
        parent->screen[y * parent->display.height + x] = color;
    }
  }
}

void TextRenderer::DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  int maxHeight = (parent->screen == parent->screenleft ? 400 : 320);
  for (u16 x = xl; x < xh; x++) {
    if (yl < maxHeight && x < (u16)parent->display.width)
      parent->screen[yl * parent->display.height + x] = color;
    if (yh - 1 < maxHeight && x < (u16)parent->display.width)
      parent->screen[(yh - 1) * parent->display.height + x] = color;
  }
  for (u16 y = yl; y < yh; y++) {
    if (y < maxHeight && xl < (u16)parent->display.width)
      parent->screen[y * parent->display.height + xl] = color;
    if (y < maxHeight && xh - 1 < (u16)parent->display.width)
      parent->screen[y * parent->display.height + xh - 1] = color;
  }
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

void TextRenderer::SetColorMode(int state) { colorMode = state; }

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
  u16 bx, by, width, height = 0;
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

  int maxY = (parent->screen == parent->screenleft) ? 400 : 320;
  int bottomClip = parent->margin.bottom;
  if (face == parent->GetFace(TEXT_STYLE_BROWSER))
    bottomClip = 0;
  if (pen.y > maxY - bottomClip) {
    pen.x += advance;
    codeprev = ucs;
    return;
  }

  MarkCurrentScreenDirtyRect((int)pen.x + (int)bx, (int)pen.y - (int)by,
                             (int)pen.x + (int)bx + (int)width,
                             (int)pen.y - (int)by + (int)height);

  for (u16 gy = 0; gy < height; gy++) {
    for (u16 gx = 0; gx < width; gx++) {
      u8 a = buffer[gy * width + gx];
      if (!a)
        continue;
      u16 sx = (pen.x + gx + bx);
      u16 sy = (pen.y + gy - by);
      if (sy >= (u16)maxY || sx >= parent->display.width)
        continue;
      const size_t dst_index =
          (size_t)sy * (size_t)parent->display.height + (size_t)sx;

      u16 pixel;
      if (colorMode == 0) {
        pixel = BlendRGB565(0x0000, parent->screen[dst_index], a);
      } else if (colorMode == 1) {
        pixel = BlendRGB565(0xFFFF, parent->screen[dst_index], a);
      } else {
        pixel = BlendRGB565(kSepiaTextColor, parent->screen[dst_index], a);
      }
      parent->screen[dst_index] = pixel;
    }
  }

  pen.x += advance;
  codeprev = ucs;
}

bool TextRenderer::PrintNewLine() {
  pen.x = parent->margin.left;
  int height = parent->GetHeight();
  int maxHeight = (parent->screen == parent->screenleft) ? 400 : 320;
  int y = pen.y + height + parent->linespacing;
  if (y > (maxHeight - parent->margin.bottom)) {
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

void TextRenderer::ClearScreen(u16 *screen, u8 r, u8 g, u8 b) {
  const int logicalHeight =
      (screen == parent->screenright)
          ? framebuffer_blit_utils::LogicalTextScreenHeight(false)
          : parent->display.height;
  MarkScreenDirtyRect(screen, 0, 0, parent->display.width, logicalHeight);
  u16 pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  for (int i = 0; i < parent->display.width * parent->display.height; i++)
    screen[i] = pixel;
}

bool TextRenderer::EnsureSplashLoaded() {
  if (splash_attempted)
    return splash_loaded;
  splash_attempted = true;

  static const char *kSplashCandidates[] = {
      paths::kSplashPaths[0],
      paths::kSplashPaths[1],
      "romfs:/3ds/3dslibris/resources/splash.jpg",
      "romfs:/3ds/3dslibris/resources/splash.jpeg",
      paths::kSplashPaths[2],
      paths::kSplashPaths[3],
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

  const int targetW = parent->display.width;
  const int targetH = parent->display.height;
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
  SetColorMode(0);
  ClearScreen();
  if (screen == parent->screenleft && EnsureSplashLoaded()) {
    for (int y = 0; y < parent->display.height; y++) {
      u16 *dst = screen + y * parent->display.height;
      const u16 *src = splash_pixels + y * parent->display.width;
      for (int x = 0; x < parent->display.width; x++)
        dst[x] = src[x];
    }
  } else if (screen == parent->screenleft) {
    DrawFallbackSplash();
  }
  MarkScreenDirty(screen);
  SetColorMode(savedColorMode);
  SetScreen(s);
}

void TextRenderer::MarkScreenDirty(u16 *target) {
  if (target == parent->screenleft) {
    parent->screenleft_dirty = true;
    parent->screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
        0, 0, parent->display.width,
        framebuffer_blit_utils::LogicalTextScreenHeight(true));
  } else if (target == parent->screenright) {
    parent->screenright_dirty = true;
    parent->screenright_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
        0, 0, parent->display.width,
        framebuffer_blit_utils::LogicalTextScreenHeight(false));
  }
}

void TextRenderer::MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1,
                                       int y1) {
  framebuffer_blit_utils::DirtyRect *dirty_rect = nullptr;
  int logical_height = parent->display.height;
  if (target == parent->screenleft) {
    parent->screenleft_dirty = true;
    dirty_rect = &parent->screenleft_dirty_rect;
    logical_height = framebuffer_blit_utils::LogicalTextScreenHeight(true);
  } else if (target == parent->screenright) {
    parent->screenright_dirty = true;
    dirty_rect = &parent->screenright_dirty_rect;
    logical_height = framebuffer_blit_utils::LogicalTextScreenHeight(false);
  } else {
    return;
  }

  framebuffer_blit_utils::ExpandDirtyRect(dirty_rect, x0, y0, x1, y1,
                                          parent->display.width, logical_height);
}

void TextRenderer::MarkCurrentScreenDirty() { MarkScreenDirty(parent->screen); }

void TextRenderer::MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1) {
  MarkScreenDirtyRect(parent->screen, x0, y0, x1, y1);
}

void TextRenderer::MarkAllScreensDirty() {
  parent->screenleft_dirty = true;
  parent->screenright_dirty = true;
  parent->screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, parent->display.width,
      framebuffer_blit_utils::LogicalTextScreenHeight(true));
  parent->screenright_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, parent->display.width,
      framebuffer_blit_utils::LogicalTextScreenHeight(false));
}

bool TextRenderer::HasDirtyScreens() const {
  return parent->screenleft_dirty || parent->screenright_dirty;
}

bool TextRenderer::BlitToFramebuffer() {
  u16 fbW = 0, fbH = 0;
  bool wrote_anything = false;

  auto blitPage = [&](u8 *fb, std::vector<u8> &cache, u16 *src,
                      u16 logicalHeight, bool dirty,
                      framebuffer_blit_utils::DirtyRect &dirty_rect,
                      u64 &cache_generation,
                      framebuffer_blit_utils::PhysicalFramebufferSyncState &sync) {
    if (!fb || !src)
      return;

    const framebuffer_blit_utils::FramebufferGeometry geometry =
        framebuffer_blit_utils::MakeFramebufferGeometry((int)fbW, (int)fbH);
    if (geometry.stride <= 0 || geometry.phys_width <= 0 ||
        geometry.byte_size == 0)
      return;

    if (cache.size() != geometry.byte_size) {
      cache.assign(geometry.byte_size, 0xFF);
      dirty = true;
      dirty_rect =
          framebuffer_blit_utils::MakeDirtyRect(0, 0, parent->display.width,
                                                logicalHeight);
    }

    if (dirty) {
      if (dirty_rect.valid) {
        framebuffer_blit_utils::ConvertLogicalRgb565RectToPhysicalBgr888(
            cache.data(), geometry, src, parent->display.height,
            parent->display.width, logicalHeight, turned_right, dirty_rect);
      } else {
        framebuffer_blit_utils::ConvertLogicalRgb565ToPhysicalBgr888(
            cache.data(), geometry, src, parent->display.height,
            parent->display.width, logicalHeight, turned_right);
      }
      cache_generation++;
    }

    const int slot =
        framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb);
    if (framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot,
                                                             cache_generation)) {
      memcpy(fb, cache.data(), geometry.byte_size);
      framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb,
                                                            cache_generation);
      wrote_anything = true;
    }
  };

  u8 *fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);
  blitPage(fbBottom, parent->screenright_fb_cache, parent->screenright,
           framebuffer_blit_utils::LogicalTextScreenHeight(false),
           parent->screenright_dirty, parent->screenright_dirty_rect,
           parent->screenright_cache_generation, parent->screenright_hw_sync);

  u8 *fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
  blitPage(fbTop, parent->screenleft_fb_cache, parent->screenleft,
           framebuffer_blit_utils::LogicalTextScreenHeight(true),
           parent->screenleft_dirty, parent->screenleft_dirty_rect,
           parent->screenleft_cache_generation, parent->screenleft_hw_sync);

  parent->screenright_dirty = false;
  parent->screenleft_dirty = false;
  parent->screenright_dirty_rect.valid = false;
  parent->screenleft_dirty_rect.valid = false;
  return wrote_anything;
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

void TextRenderer::InitPen() {
  pen.x = parent->margin.left;
  pen.y = parent->margin.top + parent->GetHeight();
}
