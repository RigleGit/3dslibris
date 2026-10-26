// Screen surface operations: gradient fills, pixel blending, rect clears, dirty
// rect tracking, and framebuffer blit. Glyph rendering is in text_renderer.cpp.

#include "ui/text_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "3ds.h"
#include "shared/bugfix_utils.h"
#include "shared/color_utils.h"
#include "shared/debug_log.h"
#include "shared/orientation_utils.h"
#include "ui/frame_debug_utils.h"
#include "ui/screen_layout_constants.h"
#include "ui/text.h"
#include "ui/text_buffer_utils.h"
#include "ui/theme_colors.h"
#include "string.h"

namespace {

static inline u16 ThemeGradientPixel(int x, int y, int w, int h,
                                     const ThemePalette &p) {
  const float tY = (h > 1) ? ((float)y / (float)(h - 1)) : 0.0f;
  const float dx =
      (w > 1) ? (((float)x - (float)(w - 1) * 0.5f) / ((float)(w - 1) * 0.5f))
              : 0.0f;
  const float edge = fabsf(dx);

  float r = p.bgTopR + (p.bgBotR - p.bgTopR) * tY;
  float g = p.bgTopG + (p.bgBotG - p.bgTopG) * tY;
  float b = p.bgTopB + (p.bgBotB - p.bgTopB) * tY;

  const float vignette = 1.0f - 0.12f * powf(edge, 1.8f);
  r *= vignette;
  g *= vignette;
  b *= vignette;
  return RGB565FromU8(r, g, b);
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

static const u16 kTrueDarkTextColor = RGB565FromU8(180.0f, 180.0f, 180.0f);
static const u16 kDarkSepiaTextColor = RGB565FromU8(180.0f, 150.0f, 110.0f);
static const u16 kDarkSepiaBgMidColor = RGB565FromU8(65.0f, 45.0f, 30.0f);

struct GradientCache {
  std::vector<u16> pixels;
  int width;
  int height;

  GradientCache() : pixels(), width(0), height(0) {}
};

// Keep two bounded slots per theme, replacing a slot when geometry changes.
static GradientCache sepia_gradients[2];
static GradientCache dark_sepia_gradients[2];
#ifdef DSLIBRIS_DEBUG
static int g_blit_geometry_diag_budget = 12;
static int g_blit_page_diag_budget = 48;
#endif

static inline u16 DarkSepiaGradientPixel(int x, int y, int w, int h) {
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

  float r = 80.0f + (50.0f - 80.0f) * tY;
  float g = 60.0f + (35.0f - 60.0f) * tY;
  float b = 40.0f + (20.0f - 40.0f) * tY;

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

static const std::vector<u16> &EnsureSepiaGradient(int w, int h, bool dark) {
  const int slot = (std::max(w, h) <= 320) ? 0 : 1;
  GradientCache &cache = dark ? dark_sepia_gradients[slot]
                              : sepia_gradients[slot];
  if (cache.pixels.empty() || cache.width != w || cache.height != h) {
    cache.pixels.resize((size_t)w * (size_t)h);
    cache.width = w;
    cache.height = h;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        cache.pixels[(size_t)y * (size_t)w + (size_t)x] =
            dark ? DarkSepiaGradientPixel(x, y, w, h)
                 : SepiaGradientPixel(x, y, w, h);
      }
    }
  }
  return cache.pixels;
}

static void FillSepiaGradient(u16 *dst, int stride, int w, int h, bool dark) {
  if (!dst || stride <= 0 || w <= 0 || h <= 0)
    return;
  const std::vector<u16> &grad = EnsureSepiaGradient(w, h, dark);
  for (int y = 0; y < h; y++) {
    memcpy(dst + (size_t)y * (size_t)stride,
           grad.data() + (size_t)y * (size_t)w, (size_t)w * sizeof(u16));
  }
}

}  // anonymous namespace

void TextRenderer::CopyScreen(u16 *src, u16 *dst) {
  memcpy(dst, src,
         (size_t)parent->BufferStride() * (size_t)parent->BufferStride() *
             sizeof(u16));
}

void TextRenderer::ClearScreen() {
  const bool is_left_screen = (parent->screen == parent->screenleft);
  const int logicalHeight = parent->LogicalHeightFor(is_left_screen);
  const int logicalWidth = parent->LogicalWidthFor(is_left_screen);
  MarkCurrentScreenDirtyRect(0, 0, logicalWidth, logicalHeight);

  if (colorMode == 2 || colorMode == 5) {
    FillSepiaGradient(parent->screen, parent->BufferStride(), logicalWidth,
                      logicalHeight, colorMode == 5);
    return;
  }

  u16 bg_color = (colorMode == 1 || colorMode == 4) ? 0x0000 : 0xFFFF;
  text_buffer_utils::FillLogicalScreenRows(parent->screen, parent->BufferStride(),
                                           logicalWidth, logicalHeight,
                                           bg_color);
}

void TextRenderer::ClearRect(u16 xl, u16 yl, u16 xh, u16 yh) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  int maxHeight = ScreenHeightPx(parent->screen, parent);
  const int maxWidth = parent->LogicalWidth();
  // Pre-clamp to valid range
  if (xl >= (u16)maxWidth || yl >= (u16)maxHeight) return;
  xh = std::min((int)xh, maxWidth);
  yh = std::min((int)yh, (int)maxHeight);
  if (xh <= xl || yh <= yl) return;
  // Gradient modes: copy from cached gradient instead of per-pixel powf
  if (colorMode == 2 || colorMode == 5) {
    const std::vector<u16> &grad =
        EnsureSepiaGradient(maxWidth, maxHeight, colorMode == 5);
    const int rw = (int)xh - (int)xl;
    const int rh = (int)yh - (int)yl;
    for (int y = 0; y < rh; y++) {
      memcpy(parent->screen + (size_t)(yl + y) * (size_t)parent->BufferStride() + (size_t)xl,
             grad.data() + (size_t)(yl + y) * (size_t)maxWidth + (size_t)xl,
             (size_t)rw * sizeof(u16));
    }
    return;
  }
  const ThemePalette &palette = GetThemePalette(colorMode);
  for (u16 y = yl; y < yh; y++) {
    u16 *row = parent->screen + (size_t)y * (size_t)parent->BufferStride() + (size_t)xl;
    for (u16 x = xl; x < xh; x++) {
      *row++ = ThemeGradientPixel(x, y, maxWidth, maxHeight, palette);
    }
  }
}

u16 TextRenderer::GetFgColor() {
  if (parent->usefgcolor)
    return parent->fgcolor;
  switch (colorMode) {
  case 1:
    return 0xFFFF;
  case 2:
    return kSepiaTextColor;
  case 4:
    return kTrueDarkTextColor;
  case 5:
    return kDarkSepiaTextColor;
  default:
    return 0x0000;
  }
}

u16 TextRenderer::GetBgColor() {
  switch (colorMode) {
  case 1:
  case 4:
    return 0x0000;
  case 2:
    return kSepiaBgMidColor;
  case 5:
    return kDarkSepiaBgMidColor;
  default:
    return 0xFFFF;
  }
}

void TextRenderer::FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  int maxH = ScreenHeightPx(parent->screen, parent);
  const int maxW = parent->LogicalWidth();
  // Pre-clamp to valid range
  if (xl >= (u16)maxW || yl >= (u16)maxH) return;
  xh = std::min((int)xh, maxW);
  yh = std::min((int)yh, (int)maxH);
  const int stride = parent->BufferStride();
  for (u16 y = yl; y < (u16)yh; y++) {
    u16 *row = parent->screen + (size_t)y * (size_t)stride + (size_t)xl;
    for (u16 x = xl; x < (u16)xh; x++) {
      *row++ = color;
    }
  }
}

void TextRenderer::DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  MarkCurrentScreenDirtyRect((int)xl, (int)yl, (int)xh, (int)yh);
  int maxHeight = ScreenHeightPx(parent->screen, parent);
  const u16 maxWidth = (u16)parent->LogicalWidth();
  const int stride = parent->BufferStride();
  for (u16 x = xl; x < xh; x++) {
    if (yl < maxHeight && x < maxWidth)
      parent->screen[yl * stride + x] = color;
    if (yh - 1 < maxHeight && x < maxWidth)
      parent->screen[(yh - 1) * stride + x] = color;
  }
  for (u16 y = yl; y < yh; y++) {
    if (y < maxHeight && xl < maxWidth)
      parent->screen[y * stride + xl] = color;
    if (y < maxHeight && xh - 1 < maxWidth)
      parent->screen[y * stride + xh - 1] = color;
  }
}

void TextRenderer::ClearScreen(u16 *screen, u8 r, u8 g, u8 b) {
  const bool is_left_like = (screen != parent->screenright);
  const int logicalHeight = parent->LogicalHeightFor(is_left_like);
  MarkScreenDirtyRect(screen, 0, 0, parent->LogicalWidthFor(is_left_like),
                      logicalHeight);
  u16 pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  const int count = parent->BufferStride() * parent->BufferStride();
  for (int i = 0; i < count; i++)
    screen[i] = pixel;
}

void TextRenderer::MarkScreenDirty(u16 *target) {
  if (target == parent->screenleft) {
    if (parent->wide_top_valid_)
      parent->wide_top_ui_dirty_ = framebuffer_blit_utils::MakeDirtyRect(
          0, 0, parent->LogicalWidthFor(true), parent->LogicalHeightFor(true));
    parent->screenleft_dirty = true;
    parent->screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
        0, 0, parent->LogicalWidthFor(true), parent->LogicalHeightFor(true));
  } else if (target == parent->screenright) {
    parent->screenright_dirty = true;
    parent->screenright_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
        0, 0, parent->LogicalWidthFor(false), parent->LogicalHeightFor(false));
  }
}

void TextRenderer::MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1,
                                       int y1) {
  framebuffer_blit_utils::DirtyRect *dirty_rect = nullptr;
  bool is_left = true;
  if (target == parent->screenleft) {
    if (parent->wide_top_valid_)
      framebuffer_blit_utils::ExpandDirtyRect(&parent->wide_top_ui_dirty_,
          x0, y0, x1, y1, parent->LogicalWidthFor(true), parent->LogicalHeightFor(true));
    parent->screenleft_dirty = true;
    dirty_rect = &parent->screenleft_dirty_rect;
  } else if (target == parent->screenright) {
    parent->screenright_dirty = true;
    dirty_rect = &parent->screenright_dirty_rect;
    is_left = false;
  } else {
    return;
  }

  framebuffer_blit_utils::ExpandDirtyRect(dirty_rect, x0, y0, x1, y1,
                                          parent->LogicalWidthFor(is_left),
                                          parent->LogicalHeightFor(is_left));
}

void TextRenderer::MarkCurrentScreenDirty() { MarkScreenDirty(parent->screen); }

void TextRenderer::MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1) {
  MarkScreenDirtyRect(parent->screen, x0, y0, x1, y1);
}

void TextRenderer::MarkAllScreensDirty() {
  parent->screenleft_dirty = true;
  parent->screenright_dirty = true;
  parent->screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, parent->LogicalWidthFor(true), parent->LogicalHeightFor(true));
  parent->screenright_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, parent->LogicalWidthFor(false), parent->LogicalHeightFor(false));
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
                      framebuffer_blit_utils::PhysicalFramebufferSyncState &sync,
                      const char *tag) {
    if (!fb || !src)
      return;

    const bool is_top = src == parent->screenleft;
    if (is_top)
      parent->SyncWideTopUiRect();
    const bool hardware_wide = is_top && parent->wide_top_fb_cache_;
    const bool native_wide = is_top && parent->wide_top_pixels_ && parent->wide_top_valid_;
    const bool landscape = orientation_utils::IsLandscape(orientation_);
    int logicalWidth = parent->LogicalWidthFor(is_top);
    int source_stride = parent->BufferStride();
    if (native_wide) {
      src = parent->wide_top_pixels_;
      logicalWidth *= landscape ? 2 : 1;
      logicalHeight *= landscape ? 1 : 2;
      source_stride = logicalWidth;
    }
    const framebuffer_blit_utils::FramebufferGeometry geometry =
        framebuffer_blit_utils::MakeFramebufferGeometry((int)fbW, (int)fbH);
    if (geometry.stride <= 0 || geometry.phys_width <= 0 ||
        geometry.byte_size == 0)
      return;
#ifdef DSLIBRIS_DEBUG
    const int max_sx = std::min(logicalWidth, geometry.stride);
    if (parent->reporter_ && g_blit_geometry_diag_budget > 0 &&
        (max_sx < logicalWidth ||
         (dirty && dirty_rect.valid && dirty_rect.x1 > max_sx))) {
      DBG_LOGF(parent->reporter_,
               "BLIT geom fb=%ux%u stride=%d phys_w=%d logical=%dx%d dirty=%d rect=%d,%d..%d,%d max_sx=%d",
               (unsigned)fbW, (unsigned)fbH, geometry.stride,
               geometry.phys_width, logicalWidth, (int)logicalHeight,
               dirty ? 1 : 0, dirty_rect.x0, dirty_rect.y0, dirty_rect.x1,
               dirty_rect.y1, max_sx);
      g_blit_geometry_diag_budget--;
    }
#endif

    if (hardware_wide && geometry.byte_size != 240u * 800u * 3u)
      return;
    if (!hardware_wide && cache.size() != geometry.byte_size) {
      cache.assign(geometry.byte_size, 0xFF);
      dirty = true;
      dirty_rect =
          framebuffer_blit_utils::MakeDirtyRect(0, 0, logicalWidth,
                                                logicalHeight);
    }

    u8 *conversion = hardware_wide ? parent->wide_top_fb_cache_ : cache.data();
    if (dirty) {
      if (dirty_rect.valid && !native_wide) {
        framebuffer_blit_utils::ConvertLogicalRgb565RectToPhysicalBgr888(
            conversion, geometry, src, source_stride,
            logicalWidth, logicalHeight, orientation_, dirty_rect);
      } else {
        framebuffer_blit_utils::ConvertLogicalRgb565ToPhysicalBgr888(
            conversion, geometry, src, source_stride,
            logicalWidth, logicalHeight, orientation_);
      }
      cache_generation++;
    }

    const int slot =
        framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb);
    const bool needs_copy = framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(
        sync, slot, cache_generation);
    if (needs_copy) {
      memcpy(fb, conversion, geometry.byte_size);
      framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb,
                                                            cache_generation);
      wrote_anything = true;
    }
#ifdef DSLIBRIS_DEBUG
    if (parent->reporter_ && g_blit_page_diag_budget > 0 &&
        frame_debug_utils::ShouldLogBlitPage(dirty, needs_copy)) {
      const uint16_t px0 = src[0];
      const uint16_t px1 = src[(size_t)std::min(10, geometry.stride - 1)];
      DBG_LOGF(parent->reporter_,
               "BLIT page=%s dirty=%d rect_valid=%d rect=%d,%d..%d,%d gen=%llu slot=%d needs_copy=%d wrote_any=%d fb=%ux%u src0=%04x src1=%04x",
               tag ? tag : "?", dirty ? 1 : 0, dirty_rect.valid ? 1 : 0,
               dirty_rect.x0, dirty_rect.y0, dirty_rect.x1, dirty_rect.y1,
               (unsigned long long)cache_generation, slot, needs_copy ? 1 : 0,
               wrote_anything ? 1 : 0, (unsigned)fbW, (unsigned)fbH,
               (unsigned)px0, (unsigned)px1);
      g_blit_page_diag_budget--;
    }
#endif
  };

  u8 *fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);
  blitPage(fbBottom, parent->screenright_fb_cache, parent->screenright,
           (u16)parent->LogicalHeightFor(false),
           parent->screenright_dirty, parent->screenright_dirty_rect,
           parent->screenright_cache_generation, parent->screenright_hw_sync,
           "bottom/right");

  u8 *fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
  blitPage(fbTop, parent->screenleft_fb_cache, parent->screenleft,
           (u16)parent->LogicalHeightFor(true),
           parent->screenleft_dirty, parent->screenleft_dirty_rect,
           parent->screenleft_cache_generation, parent->screenleft_hw_sync,
           "top/left");

  parent->screenright_dirty = false;
  parent->screenleft_dirty = false;
  parent->screenright_dirty_rect.valid = false;
  parent->screenleft_dirty_rect.valid = false;
  return wrote_anything;
}
