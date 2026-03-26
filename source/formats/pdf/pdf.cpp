// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/pdf/pdf.h"

#include "app/app.h"
#include "book/book.h"
#include "book/page.h"
#include "formats/common/book_error.h"
#include "main.h"
#include "shared/pdf_view_utils.h"
#include "ui/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "mupdf/fitz/color.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/device.h"
#include "mupdf/fitz/display-list.h"
#include "mupdf/fitz/document.h"
#include "mupdf/fitz/geometry.h"
#include "mupdf/fitz/outline.h"
#include "mupdf/fitz/pixmap.h"
#include "mupdf/fitz/util.h"
#include "mupdf/pdf/document.h"
}

extern App *app;

namespace {

static const int kPdfPreviewScreenWidth = 240;
static const int kPdfPreviewScreenHeight = 320;
static const int kPdfPreviewPadding = 4;
static const int kPdfZoomScreenWidth = 240;
static const int kPdfZoomScreenHeight = 400;
static const u16 kPdfPaper = 0xFFFF;
static const u16 kPdfFrame = 0x2104;
static const u16 kPdfAccent = 0x0000;
static const float kPdfReadingBaseZoom = 1.5f;

// Pre-computed lookup table for fast grayscale → RGB565 conversion.
// Manga PDFs are B&W so grayscale rendering is visually identical but
// reduces MuPDF rasterization bandwidth by ~3x.
static u16 kGrayToRgb565[256];
static bool kGrayLutReady = false;
static void EnsureGrayLut() {
  if (kGrayLutReady) return;
  for (int g = 0; g < 256; g++) {
    kGrayToRgb565[g] = (u16)(((u16)(g >> 3) << 11) |
                             ((u16)(g >> 2) << 5) |
                             (u16)(g >> 3));
  }
  kGrayLutReady = true;
}

static bool DetectNew3ds() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

struct RenderedPdfBitmap {
  int width;
  int height;
  std::vector<u16> pixels;

  RenderedPdfBitmap() : width(0), height(0), pixels() {}
};

struct BitmapBoundsPx {
  int left;
  int top;
  int width;
  int height;

  BitmapBoundsPx() : left(0), top(0), width(0), height(0) {}
};

struct PdfNavigationBounds {
  float left;
  float top;
  float width;
  float height;

  PdfNavigationBounds() : left(0.0f), top(0.0f), width(1.0f), height(1.0f) {}
};

static void ResetBitmapCache(Book::PdfState::BitmapCache *cache) {
  if (!cache)
    return;
  cache->page = -1;
  cache->zoom_index = -1;
  cache->left = 0.0f;
  cache->top = 0.0f;
  cache->width = 1.0f;
  cache->height = 1.0f;
  cache->bitmap_width = 0;
  cache->bitmap_height = 0;
  cache->pixels.clear();
}

static void ResetAdjacentSlot(Book::PdfState::AdjacentSlot *slot,
                              fz_context *ctx) {
  if (!slot)
    return;
  if (slot->display_list && ctx)
    fz_drop_display_list(ctx, slot->display_list);
  slot->page = -1;
  slot->display_list = NULL;
  ResetBitmapCache(&slot->preview);
  ResetBitmapCache(&slot->interactive_tile);
}

static void StoreBitmapCache(Book::PdfState::BitmapCache *cache, int page,
                             int zoom_index, float left, float top,
                             float width, float height,
                             RenderedPdfBitmap *rendered) {
  if (!cache || !rendered)
    return;
  cache->page = page;
  cache->zoom_index = zoom_index;
  cache->left = left;
  cache->top = top;
  cache->width = width;
  cache->height = height;
  cache->bitmap_width = rendered->width;
  cache->bitmap_height = rendered->height;
  cache->pixels.swap(rendered->pixels);
}

static bool BitmapCacheValid(const Book::PdfState::BitmapCache &cache,
                             int page) {
  return cache.page == page && cache.bitmap_width > 0 &&
         cache.bitmap_height > 0 && !cache.pixels.empty();
}

static bool BitmapCacheCoversRect(const Book::PdfState::BitmapCache &cache,
                                  int page, int zoom_index,
                                  const pdf_view_utils::NormalizedRect &rect) {
  const float eps = 0.001f;
  return BitmapCacheValid(cache, page) && cache.zoom_index == zoom_index &&
         cache.left <= rect.left + eps && cache.top <= rect.top + eps &&
         cache.left + cache.width >= rect.left + rect.width - eps &&
         cache.top + cache.height >= rect.top + rect.height - eps;
}

static u16 RGB565FromRgb8(unsigned char r, unsigned char g, unsigned char b) {
  return (u16)(((u16)(r >> 3) << 11) | ((u16)(g >> 2) << 5) | (u16)(b >> 3));
}

static void RGB565ToRgb8(u16 pixel, int *r, int *g, int *b) {
  if (!r || !g || !b)
    return;
  const int r5 = (pixel >> 11) & 0x1F;
  const int g6 = (pixel >> 5) & 0x3F;
  const int b5 = pixel & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

static bool IsMostlyWhite(u16 pixel) {
  int r = 0;
  int g = 0;
  int b = 0;
  RGB565ToRgb8(pixel, &r, &g, &b);
  return (r >= 248 && g >= 248 && b >= 248);
}

static float ComputeFitScale(float page_width, float page_height,
                             int target_width, int target_height) {
  if (page_width <= 0.0f || page_height <= 0.0f || target_width <= 0 ||
      target_height <= 0) {
    return 1.0f;
  }
  const float sx = (float)target_width / page_width;
  const float sy = (float)target_height / page_height;
  const float scale = std::min(sx, sy);
  return (scale > 0.0f) ? scale : 1.0f;
}

static float ComputeEffectivePdfZoom(int zoom_index) {
  return pdf_view_utils::ZoomForIndex(zoom_index) * kPdfReadingBaseZoom;
}

static PdfNavigationBounds GetPdfNavigationBounds(float content_left,
                                                  float content_top,
                                                  float content_width,
                                                  float content_height) {
  PdfNavigationBounds bounds;
  const float margin = 0.03f;
  const float left = std::max(0.0f, content_left - margin);
  const float top = std::max(0.0f, content_top - margin);
  const float right =
      std::min(1.0f, content_left + content_width + margin);
  const float bottom =
      std::min(1.0f, content_top + content_height + margin);

  if (right > left && bottom > top &&
      content_width > 0.05f && content_height > 0.05f) {
    bounds.left = left;
    bounds.top = top;
    bounds.width = right - left;
    bounds.height = bottom - top;
  }
  return bounds;
}

static pdf_view_utils::NormalizedRect ComputePdfViewportRect(
    float page_width, float page_height, int zoom_index, float center_x,
    float center_y, float content_left, float content_top, float content_width,
    float content_height) {
  pdf_view_utils::NormalizedRect out = {0.0f, 0.0f, 1.0f, 1.0f};
  const PdfNavigationBounds nav =
      GetPdfNavigationBounds(content_left, content_top, content_width,
                             content_height);
  const float local_center_x =
      std::max(0.0f, std::min(1.0f, (center_x - nav.left) /
                                        std::max(0.0001f, nav.width)));
  const float local_center_y =
      std::max(0.0f, std::min(1.0f, (center_y - nav.top) /
                                        std::max(0.0001f, nav.height)));
  const pdf_view_utils::NormalizedRect local =
      pdf_view_utils::ComputeViewportRect(
          std::max(1.0f, page_width * nav.width),
          std::max(1.0f, page_height * nav.height),
          ComputeEffectivePdfZoom(zoom_index), (float)kPdfZoomScreenWidth,
          (float)kPdfZoomScreenHeight, local_center_x, local_center_y);

  out.left = nav.left + local.left * nav.width;
  out.top = nav.top + local.top * nav.height;
  out.width = local.width * nav.width;
  out.height = local.height * nav.height;
  return out;
}

static int ClampPdfPageIndex(int page_index, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (page_index < 0)
    return 0;
  if (page_index >= (int)page_count)
    return (int)page_count - 1;
  return page_index;
}

static void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                       int x, int y, int draw_width,
                                       int draw_height,
                                       const std::vector<u16> &pixels,
                                       int src_width, int src_height,
                                       int crop_x, int crop_y, int crop_width,
                                       int crop_height) {
  if (!ts || !screen || pixels.empty() || draw_width <= 0 || draw_height <= 0 ||
      src_width <= 0 || src_height <= 0 || crop_width <= 0 || crop_height <= 0) {
    return;
  }
  crop_x = std::max(0, std::min(src_width - 1, crop_x));
  crop_y = std::max(0, std::min(src_height - 1, crop_y));
  crop_width = std::max(1, std::min(src_width - crop_x, crop_width));
  crop_height = std::max(1, std::min(src_height - crop_y, crop_height));

  const int stride = ts->display.height;
  const int logical_width = ts->display.width;
  ts->MarkScreenDirty(screen);
  for (int row = 0; row < draw_height; row++) {
    const int dy = y + row;
    if (dy < 0 || dy >= logical_height)
      continue;
    const int src_y =
        crop_y + std::min(crop_height - 1, (row * crop_height) / draw_height);
    for (int col = 0; col < draw_width; col++) {
      const int dx = x + col;
      if (dx < 0 || dx >= logical_width)
        continue;
      const int src_x =
          crop_x + std::min(crop_width - 1, (col * crop_width) / draw_width);
      screen[(size_t)dy * (size_t)stride + (size_t)dx] =
          pixels[(size_t)src_y * (size_t)src_width + (size_t)src_x];
    }
  }
}

static void ComputeBitmapContentBoundsPx(const RenderedPdfBitmap &bitmap,
                                         BitmapBoundsPx *bounds) {
  if (!bounds)
    return;
  bounds->left = 0;
  bounds->top = 0;
  bounds->width = std::max(0, bitmap.width);
  bounds->height = std::max(0, bitmap.height);
  if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty())
    return;

  const int w = bitmap.width;
  const int h = bitmap.height;

  // Optimized border-shrinking scan: find first/last non-white row/column
  // instead of scanning every pixel.  O(W+H) typical vs O(W*H).
  int min_y = h;
  for (int y = 0; y < h && min_y == h; y++) {
    const u16 *row = bitmap.pixels.data() + (size_t)y * (size_t)w;
    for (int x = 0; x < w; x++) {
      if (!IsMostlyWhite(row[x])) { min_y = y; break; }
    }
  }
  if (min_y == h)
    return; // entirely white

  int max_y = min_y;
  for (int y = h - 1; y > min_y; y--) {
    const u16 *row = bitmap.pixels.data() + (size_t)y * (size_t)w;
    for (int x = 0; x < w; x++) {
      if (!IsMostlyWhite(row[x])) { max_y = y; break; }
    }
    if (max_y > min_y) break;
  }

  int min_x = w;
  int max_x = 0;
  for (int y = min_y; y <= max_y; y++) {
    const u16 *row = bitmap.pixels.data() + (size_t)y * (size_t)w;
    // Scan from left for this row
    for (int x = 0; x < min_x; x++) {
      if (!IsMostlyWhite(row[x])) { min_x = x; break; }
    }
    // Scan from right for this row
    for (int x = w - 1; x > max_x; x--) {
      if (!IsMostlyWhite(row[x])) { max_x = x; break; }
    }
    if (min_x == 0 && max_x == w - 1)
      break; // already at full width
  }

  bounds->left = min_x;
  bounds->top = min_y;
  bounds->width = max_x - min_x + 1;
  bounds->height = max_y - min_y + 1;
}

static void ComputeBitmapContentBoundsNormalized(const RenderedPdfBitmap &bitmap,
                                                 float *left, float *top,
                                                 float *width, float *height) {
  if (!left || !top || !width || !height)
    return;
  if (bitmap.width <= 0 || bitmap.height <= 0) {
    *left = 0.0f;
    *top = 0.0f;
    *width = 1.0f;
    *height = 1.0f;
    return;
  }

  BitmapBoundsPx bounds;
  ComputeBitmapContentBoundsPx(bitmap, &bounds);
  *left = (float)bounds.left / (float)bitmap.width;
  *top = (float)bounds.top / (float)bitmap.height;
  *width = std::max(0.0001f, (float)bounds.width / (float)bitmap.width);
  *height = std::max(0.0001f, (float)bounds.height / (float)bitmap.height);
}

static fz_matrix MakePdfRenderMatrix(fz_rect page_bounds, float scale) {
  return fz_transform_page(page_bounds, 72.0f * scale, 0.0f);
}

static bool QueryPdfPageMetrics(fz_context *ctx, pdf_document *doc,
                                int page_index, float *page_width,
                                float *page_height) {
  if (!ctx || !doc || !page_width || !page_height)
    return false;

  fz_page *page = NULL;
  fz_rect bounds = fz_empty_rect;
  bool ok = false;

  fz_var(page);
  fz_try(ctx) {
    page = fz_load_page(ctx, (fz_document *)doc, page_index);
    bounds = fz_bound_page(ctx, page);
    ok = (bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0);
  }
  fz_always(ctx) { fz_drop_page(ctx, page); }
  fz_catch(ctx) { ok = false; }

  if (!ok)
    return false;

  *page_width = bounds.x1 - bounds.x0;
  *page_height = bounds.y1 - bounds.y0;
  return true;
}

static bool RenderPdfBitmap(fz_context *ctx, pdf_document *doc, int page_index,
                            float scale, RenderedPdfBitmap *out,
                            float *page_width, float *page_height,
                            const pdf_view_utils::NormalizedRect *crop_rect = NULL,
                            fz_display_list *reuse_list = NULL,
                            fz_display_list **out_list = NULL) {
  if (!ctx || !doc || !out || scale <= 0.0f)
    return false;

  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  fz_display_list *list = reuse_list;
  fz_rect bounds = fz_empty_rect;
  fz_rect render_rect = fz_empty_rect;
  fz_irect bbox = fz_empty_irect;
  bool ok = false;
  bool owns_list = false;

  fz_var(page);
  fz_var(pixmap);
  fz_var(device);
  fz_var(list);

  fz_try(ctx) {
    page = fz_load_page(ctx, (fz_document *)doc, page_index);
    bounds = fz_bound_page(ctx, page);
    if (!(bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0))
      fz_throw(ctx, FZ_ERROR_FORMAT, "empty pdf page bounds");

    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (page_width)
      *page_width = width;
    if (page_height)
      *page_height = height;

    render_rect = bounds;
    if (crop_rect) {
      const float crop_left = std::max(0.0f, std::min(1.0f, crop_rect->left));
      const float crop_top = std::max(0.0f, std::min(1.0f, crop_rect->top));
      const float crop_right =
          std::max(crop_left,
                   std::min(1.0f, crop_rect->left + crop_rect->width));
      const float crop_bottom =
          std::max(crop_top,
                   std::min(1.0f, crop_rect->top + crop_rect->height));
      render_rect.x0 = bounds.x0 + width * crop_left;
      render_rect.y0 = bounds.y0 + height * crop_top;
      render_rect.x1 = bounds.x0 + width * crop_right;
      render_rect.y1 = bounds.y0 + height * crop_bottom;
      if (!(render_rect.x1 > render_rect.x0 && render_rect.y1 > render_rect.y0))
        render_rect = bounds;
    }

    // Build display list if not provided (captures page ops for cheap replay).
    if (!list) {
      list = fz_new_display_list_from_page(ctx, page);
      owns_list = true;
    }

    const fz_matrix ctm = MakePdfRenderMatrix(bounds, scale);
    bbox = fz_round_rect(fz_transform_rect(render_rect, ctm));
    if (bbox.x1 <= bbox.x0 || bbox.y1 <= bbox.y0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf render bbox");

    // Grayscale rendering: 3x less memory bandwidth for MuPDF's rasterizer.
    // Manga pages are B&W, so visual quality is identical.
    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pixmap, 255);
    device = fz_new_draw_device(ctx, fz_identity, pixmap);
    fz_run_display_list(ctx, list, device, ctm, fz_infinite_rect, NULL);
    fz_close_device(ctx, device);

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);
    if (!samples || pix_w <= 0 || pix_h <= 0 || stride <= 0 || comps < 1)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf pixmap");

    EnsureGrayLut();
    out->width = pix_w;
    out->height = pix_h;
    out->pixels.resize((size_t)pix_w * (size_t)pix_h);
    u16 *dst = out->pixels.data();
    if (comps == 1) {
      // Fast path: grayscale → RGB565 via lookup table.
      for (int y = 0; y < pix_h; y++) {
        const unsigned char *src = samples + (size_t)y * (size_t)stride;
        for (int x = 0; x < pix_w; x++)
          *dst++ = kGrayToRgb565[*src++];
      }
    } else {
      // Fallback: RGB → RGB565.
      for (int y = 0; y < pix_h; y++) {
        const unsigned char *src = samples + (size_t)y * (size_t)stride;
        for (int x = 0; x < pix_w; x++) {
          *dst++ = RGB565FromRgb8(src[0], src[1], src[2]);
          src += comps;
        }
      }
    }
    ok = true;
  }
  fz_always(ctx) {
    fz_drop_device(ctx, device);
    fz_drop_pixmap(ctx, pixmap);
    fz_drop_page(ctx, page);
  }
  fz_catch(ctx) {
    out->width = 0;
    out->height = 0;
    out->pixels.clear();
    if (owns_list && list) {
      fz_drop_display_list(ctx, list);
      list = NULL;
      owns_list = false;
    }
    ok = false;
  }

  // Hand off display list to caller if requested.
  if (out_list) {
    *out_list = list;
  } else if (owns_list && list) {
    fz_drop_display_list(ctx, list);
  }

  return ok;
}

static pdf_view_utils::NormalizedPoint RecenterViewportFromRawPreview(
    const pdf_view_utils::PreviewLayout &preview,
    const pdf_view_utils::NormalizedRect &viewport,
    const PdfNavigationBounds &nav, int touch_x, int touch_y) {
  pdf_view_utils::NormalizedPoint out = {0.5f, 0.5f};
  if (preview.width <= 0 || preview.height <= 0)
    return out;

  float px = (float)(touch_x - preview.x) / (float)preview.width;
  float py = (float)(touch_y - preview.y) / (float)preview.height;
  px = std::max(0.0f, std::min(1.0f, px));
  py = std::max(0.0f, std::min(1.0f, py));

  const float min_x = nav.left + viewport.width * 0.5f;
  const float max_x = nav.left + nav.width - viewport.width * 0.5f;
  const float min_y = nav.top + viewport.height * 0.5f;
  const float max_y = nav.top + nav.height - viewport.height * 0.5f;
  out.x = std::max(min_x, std::min(max_x, px));
  out.y = std::max(min_y, std::min(max_y, py));
  return out;
}

static float ComputePdfPreviewScale(float page_width, float page_height) {
  return ComputeFitScale(page_width, page_height,
                         kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
                         kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
}

static float ComputePdfFinalScale(float page_width, float page_height,
                                  int max_zoom_index) {
  return ComputeFitScale(page_width, page_height, kPdfZoomScreenWidth,
                         kPdfZoomScreenHeight) *
         ComputeEffectivePdfZoom(max_zoom_index);
}

static float ComputePdfInteractiveScale(float page_width, float page_height,
                                        int zoom_index) {
  return ComputeFitScale(page_width, page_height, kPdfZoomScreenWidth,
                         kPdfZoomScreenHeight) *
         ComputeEffectivePdfZoom(zoom_index);
}

static void GetPreviewContentBounds(const Book::PdfState *pdf_state, float *left,
                                    float *top, float *width, float *height) {
  if (!left || !top || !width || !height)
    return;
  if (pdf_state && BitmapCacheValid(pdf_state->current_preview, pdf_state->current_preview.page)) {
    *left = pdf_state->current_preview.left;
    *top = pdf_state->current_preview.top;
    *width = pdf_state->current_preview.width;
    *height = pdf_state->current_preview.height;
    return;
  }
  *left = 0.0f;
  *top = 0.0f;
  *width = 1.0f;
  *height = 1.0f;
}

static pdf_view_utils::NormalizedRect ComputeCurrentPdfViewport(
    const Book::PdfState *pdf_state) {
  float left = 0.0f;
  float top = 0.0f;
  float width = 1.0f;
  float height = 1.0f;
  GetPreviewContentBounds(pdf_state, &left, &top, &width, &height);
  return ComputePdfViewportRect(pdf_state->page_width, pdf_state->page_height,
                                pdf_state->zoom_index,
                                pdf_state->viewport_center_x,
                                pdf_state->viewport_center_y, left, top, width,
                                height);
}

static Book::PdfState::AdjacentSlot *GetAdjacentSlot(Book::PdfState *pdf_state,
                                                     int direction) {
  if (!pdf_state)
    return NULL;
  return (direction < 0) ? &pdf_state->prev_slot : &pdf_state->next_slot;
}

static bool PromoteAdjacentSlotIfMatching(Book::PdfState *pdf_state,
                                          int page_index) {
  if (!pdf_state)
    return false;
  Book::PdfState::AdjacentSlot *slot = NULL;
  if (pdf_state->prev_slot.page == page_index)
    slot = &pdf_state->prev_slot;
  else if (pdf_state->next_slot.page == page_index)
    slot = &pdf_state->next_slot;
  if (!slot)
    return false;

  ResetBitmapCache(&pdf_state->current_preview);
  ResetBitmapCache(&pdf_state->current_interactive_tile);
  pdf_state->current_preview = slot->preview;
  pdf_state->current_interactive_tile = slot->interactive_tile;
  ResetBitmapCache(&slot->preview);
  ResetBitmapCache(&slot->interactive_tile);

  if (pdf_state->cached_display_list && pdf_state->ctx)
    fz_drop_display_list(pdf_state->ctx, pdf_state->cached_display_list);
  pdf_state->cached_display_list = slot->display_list;
  pdf_state->cached_display_list_page = page_index;
  slot->display_list = NULL;
  slot->page = -1;
  pdf_state->final_cache_pending = true;
  return true;
}

static bool EnsurePdfDisplayListForPage(Book::PdfState *pdf_state,
                                        int page_index,
                                        fz_display_list **out_list) {
  if (!pdf_state || !out_list)
    return false;
  if (pdf_state->cached_display_list_page == page_index &&
      pdf_state->cached_display_list) {
    *out_list = pdf_state->cached_display_list;
    return true;
  }
  if (PromoteAdjacentSlotIfMatching(pdf_state, page_index) &&
      pdf_state->cached_display_list_page == page_index &&
      pdf_state->cached_display_list) {
    *out_list = pdf_state->cached_display_list;
    return true;
  }

  if (pdf_state->cached_display_list && pdf_state->ctx) {
    fz_drop_display_list(pdf_state->ctx, pdf_state->cached_display_list);
    pdf_state->cached_display_list = NULL;
    pdf_state->cached_display_list_page = -1;
  }
  *out_list = NULL;
  return true;
}

static bool EnsureCurrentPreviewCache(Book::PdfState *pdf_state, int page_index) {
  if (!pdf_state)
    return false;
  PromoteAdjacentSlotIfMatching(pdf_state, page_index);
  if (BitmapCacheValid(pdf_state->current_preview, page_index))
    return true;

  fz_display_list *display_list = NULL;
  if (!EnsurePdfDisplayListForPage(pdf_state, page_index, &display_list))
    return false;

  RenderedPdfBitmap rendered;
  fz_display_list *new_list = NULL;
  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                       ComputePdfPreviewScale(page_width, page_height),
                       &rendered, &page_width, &page_height, NULL,
                       display_list, display_list ? NULL : &new_list)) {
    return false;
  }

  float left = 0.0f, top = 0.0f, width = 1.0f, height = 1.0f;
  ComputeBitmapContentBoundsNormalized(rendered, &left, &top, &width, &height);
  StoreBitmapCache(&pdf_state->current_preview, page_index, -1, left, top,
                   width, height, &rendered);
  pdf_state->page_width = page_width;
  pdf_state->page_height = page_height;
  if (new_list && !display_list) {
    pdf_state->cached_display_list = new_list;
    pdf_state->cached_display_list_page = page_index;
  }
  return true;
}

static bool EnsureCurrentInteractiveTile(Book::PdfState *pdf_state,
                                         int page_index,
                                         const pdf_view_utils::NormalizedRect &viewport) {
  if (!pdf_state)
    return false;
  PromoteAdjacentSlotIfMatching(pdf_state, page_index);
  if (BitmapCacheCoversRect(pdf_state->current_interactive_tile, page_index,
                            pdf_state->zoom_index, viewport))
    return true;

  fz_display_list *display_list = NULL;
  if (!EnsurePdfDisplayListForPage(pdf_state, page_index, &display_list))
    return false;

  RenderedPdfBitmap rendered;
  fz_display_list *new_list = NULL;
  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                       ComputePdfInteractiveScale(page_width, page_height,
                                                  pdf_state->zoom_index),
                       &rendered, &page_width, &page_height, &viewport,
                       display_list, display_list ? NULL : &new_list)) {
    return false;
  }

  StoreBitmapCache(&pdf_state->current_interactive_tile, page_index,
                   pdf_state->zoom_index, viewport.left, viewport.top,
                   viewport.width, viewport.height, &rendered);
  pdf_state->page_width = page_width;
  pdf_state->page_height = page_height;
  if (new_list && !display_list) {
    pdf_state->cached_display_list = new_list;
    pdf_state->cached_display_list_page = page_index;
  }
  return true;
}

static bool EnsureCurrentFinalCache(Book::PdfState *pdf_state, int page_index) {
  if (!pdf_state)
    return false;
  if (BitmapCacheValid(pdf_state->current_final_zoom, page_index) &&
      pdf_state->current_final_zoom.zoom_index >= pdf_state->max_zoom_index) {
    pdf_state->final_cache_pending = false;
    return true;
  }

  fz_display_list *display_list = NULL;
  if (!EnsurePdfDisplayListForPage(pdf_state, page_index, &display_list))
    return false;

  RenderedPdfBitmap rendered;
  fz_display_list *new_list = NULL;
  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                       ComputePdfFinalScale(page_width, page_height,
                                            pdf_state->max_zoom_index),
                       &rendered, &page_width, &page_height, NULL,
                       display_list, display_list ? NULL : &new_list)) {
    return false;
  }

  StoreBitmapCache(&pdf_state->current_final_zoom, page_index,
                   pdf_state->max_zoom_index, 0.0f, 0.0f, 1.0f, 1.0f,
                   &rendered);
  pdf_state->page_width = page_width;
  pdf_state->page_height = page_height;
  pdf_state->final_cache_pending = false;
  if (new_list && !display_list) {
    pdf_state->cached_display_list = new_list;
    pdf_state->cached_display_list_page = page_index;
  }
  return true;
}

static bool PrepareAdjacentPdfSlot(Book::PdfState *pdf_state, int current_page,
                                   int direction) {
  if (!pdf_state || !pdf_state->ctx || !pdf_state->doc || direction == 0)
    return false;
  const int page_index =
      ClampPdfPageIndex(current_page + (direction < 0 ? -1 : 1),
                        pdf_state->page_count);
  if (page_index == current_page)
    return false;

  Book::PdfState::AdjacentSlot *slot = GetAdjacentSlot(pdf_state, direction);
  if (!slot)
    return false;

  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentPdfViewport(pdf_state);
  if (slot->page == page_index &&
      BitmapCacheValid(slot->preview, page_index) &&
      BitmapCacheCoversRect(slot->interactive_tile, page_index,
                            pdf_state->zoom_index, viewport)) {
    return false;
  }

  ResetAdjacentSlot(slot, pdf_state->ctx);

  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  RenderedPdfBitmap preview;
  fz_display_list *display_list = NULL;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                       ComputePdfPreviewScale(page_width, page_height),
                       &preview, &page_width, &page_height, NULL, NULL,
                       &display_list) ||
      !display_list) {
    ResetAdjacentSlot(slot, pdf_state->ctx);
    return false;
  }

  float left = 0.0f, top = 0.0f, width = 1.0f, height = 1.0f;
  ComputeBitmapContentBoundsNormalized(preview, &left, &top, &width, &height);
  StoreBitmapCache(&slot->preview, page_index, -1, left, top, width, height,
                   &preview);

  RenderedPdfBitmap interactive;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                       ComputePdfInteractiveScale(page_width, page_height,
                                                  pdf_state->zoom_index),
                       &interactive, &page_width, &page_height, &viewport,
                       display_list, NULL)) {
    ResetAdjacentSlot(slot, pdf_state->ctx);
    return false;
  }
  StoreBitmapCache(&slot->interactive_tile, page_index, pdf_state->zoom_index,
                   viewport.left, viewport.top, viewport.width,
                   viewport.height, &interactive);

  slot->page = page_index;
  slot->display_list = display_list;
  return true;
}

static void AddPdfOutlineEntries(Book *book, fz_context *ctx, pdf_document *doc,
                                 const fz_outline *entry, u8 level) {
  if (!book || !ctx || !doc)
    return;

  for (const fz_outline *node = entry; node; node = node->next) {
    int page_index = -1;
    fz_try(ctx) {
      if (node->page.chapter >= 0 && node->page.page >= 0) {
        page_index =
            fz_page_number_from_location(ctx, (fz_document *)doc, node->page);
      } else if (node->uri && node->uri[0]) {
        float x = 0.0f;
        float y = 0.0f;
        fz_location loc =
            fz_resolve_link(ctx, (fz_document *)doc, node->uri, &x, &y);
        page_index = fz_page_number_from_location(ctx, (fz_document *)doc, loc);
      }
    }
    fz_catch(ctx) { page_index = -1; }

    if (page_index >= 0 && page_index <= 65535 && node->title &&
        node->title[0]) {
      book->AddChapter((u16)page_index, node->title,
                       (u8)std::min<int>(level, 7));
    }

    if (node->down)
      AddPdfOutlineEntries(book, ctx, doc, node->down, (u8)(level + 1));
  }
}

static void PopulatePdfMetadata(Book *book, fz_context *ctx, pdf_document *doc) {
  if (!book || !ctx || !doc)
    return;

  char title_buf[512];
  char author_buf[512];
  title_buf[0] = '\0';
  author_buf[0] = '\0';

  fz_try(ctx) {
    if (fz_lookup_metadata(ctx, (fz_document *)doc, FZ_META_INFO_TITLE,
                           title_buf, sizeof(title_buf)) > 0 &&
        title_buf[0]) {
      book->SetTitle(title_buf);
    }
    if (fz_lookup_metadata(ctx, (fz_document *)doc, FZ_META_INFO_AUTHOR,
                           author_buf, sizeof(author_buf)) > 0 &&
        author_buf[0]) {
      std::string author(author_buf);
      book->SetAuthor(author);
    }
  }
  fz_catch(ctx) {
  }
}

} // namespace

void Book::ResetPdfState() {
  if (!pdf_state)
    return;
  if (pdf_state->ctx) {
    if (pdf_state->cached_display_list)
      fz_drop_display_list(pdf_state->ctx, pdf_state->cached_display_list);
    ResetAdjacentSlot(&pdf_state->prev_slot, pdf_state->ctx);
    ResetAdjacentSlot(&pdf_state->next_slot, pdf_state->ctx);
    fz_drop_outline(pdf_state->ctx, pdf_state->outline);
    pdf_drop_document(pdf_state->ctx, pdf_state->doc);
    fz_drop_context(pdf_state->ctx);
  }
  delete pdf_state;
  pdf_state = NULL;
}

void Book::InitPdfView(u16 page_count, fz_context *ctx, pdf_document *doc,
                       fz_outline *outline, bool is_new_3ds) {
  ResetPdfState();
  pdf_state = new PdfState();
  const pdf_view_utils::DevicePolicy policy =
      pdf_view_utils::GetDevicePolicy(is_new_3ds);
  pdf_state->ctx = ctx;
  pdf_state->doc = doc;
  pdf_state->outline = outline;
  pdf_state->page_count = page_count;
  pdf_state->is_new_3ds = is_new_3ds;
  pdf_state->keep_preview_cache = policy.keep_preview_cache;
  pdf_state->keep_tile_cache = policy.keep_tile_cache;
  pdf_state->max_zoom_index = policy.max_zoom_index;
  pdf_state->zoom_index = policy.default_zoom_index;
  pdf_state->viewport_center_x = 0.5f;
  pdf_state->viewport_center_y = 0.5f;
  pdf_state->final_cache_pending = true;
}

bool Book::ChangePdfZoom(int delta) {
  if (!IsPdf() || !pdf_state || delta == 0)
    return false;
  const int next = std::min(
      pdf_state->max_zoom_index,
      pdf_view_utils::ClampZoomIndexForDevice(pdf_state->zoom_index + delta,
                                              pdf_state->is_new_3ds));
  if (next == pdf_state->zoom_index)
    return false;
  pdf_state->zoom_index = next;
  if (pdf_state->current_final_zoom.page != position ||
      pdf_state->current_final_zoom.zoom_index < pdf_state->max_zoom_index)
    pdf_state->final_cache_pending = true;
  return true;
}

bool Book::MovePdfViewportToPreview(int touch_x, int touch_y) {
  if (!IsPdf() || !pdf_state)
    return false;
  float preview_source_width = pdf_state->page_width;
  float preview_source_height = pdf_state->page_height;
  if (pdf_state->current_preview.bitmap_width > 0 &&
      pdf_state->current_preview.bitmap_height > 0) {
    preview_source_width = (float)pdf_state->current_preview.bitmap_width;
    preview_source_height = (float)pdf_state->current_preview.bitmap_height;
  }
  const pdf_view_utils::PreviewLayout preview =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height,
          kPdfPreviewPadding, kPdfPreviewPadding,
          kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
          kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
  const PdfNavigationBounds nav =
      GetPdfNavigationBounds(pdf_state->current_preview.left,
                             pdf_state->current_preview.top,
                             pdf_state->current_preview.width,
                             pdf_state->current_preview.height);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentPdfViewport(pdf_state);
  const pdf_view_utils::NormalizedPoint center =
      RecenterViewportFromRawPreview(preview, viewport, nav, touch_x, touch_y);
  const float dx = std::abs(center.x - pdf_state->viewport_center_x);
  const float dy = std::abs(center.y - pdf_state->viewport_center_y);
  if (dx < 0.0005f && dy < 0.0005f)
    return false;
  pdf_state->viewport_center_x = center.x;
  pdf_state->viewport_center_y = center.y;
  return true;
}

bool Book::JumpPdfChapter(int delta) {
  if (!IsPdf() || delta == 0 || chapters.empty())
    return false;
  const int current = GetPosition();
  if (delta > 0) {
    for (size_t i = 0; i < chapters.size(); i++) {
      if ((int)chapters[i].page > current) {
        SetPosition(chapters[i].page);
        return true;
      }
    }
    return false;
  }
  for (size_t i = chapters.size(); i > 0; i--) {
    if ((int)chapters[i - 1].page < current) {
      SetPosition(chapters[i - 1].page);
      return true;
    }
  }
  return false;
}

void Book::DrawCurrentView(Text *ts) {
  if (!ts)
    return;
  if (!IsPdf()) {
    if (GetPageCount() == 0)
      return;
    GetPage()->Draw(ts);
    return;
  }
  if (!pdf_state || !pdf_state->ctx || !pdf_state->doc || pdf_state->page_count == 0)
    return;

  const int page_index = ClampPdfPageIndex(position, pdf_state->page_count);
  position = page_index;

  if (pdf_state->current_preview.page != page_index)
    ResetBitmapCache(&pdf_state->current_preview);
  if (pdf_state->current_interactive_tile.page != page_index)
    ResetBitmapCache(&pdf_state->current_interactive_tile);
  if (pdf_state->current_final_zoom.page != page_index)
    ResetBitmapCache(&pdf_state->current_final_zoom);

  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  if (QueryPdfPageMetrics(pdf_state->ctx, pdf_state->doc, page_index,
                          &page_width, &page_height)) {
    pdf_state->page_width = page_width;
    pdf_state->page_height = page_height;
  }

  if (!EnsureCurrentPreviewCache(pdf_state, page_index))
    return;

  pdf_view_utils::NormalizedRect viewport = ComputeCurrentPdfViewport(pdf_state);
  pdf_state->viewport_center_x = viewport.left + viewport.width * 0.5f;
  pdf_state->viewport_center_y = viewport.top + viewport.height * 0.5f;
  const bool has_final_cache =
      BitmapCacheValid(pdf_state->current_final_zoom, page_index) &&
      pdf_state->current_final_zoom.zoom_index >= pdf_state->max_zoom_index;
  if (!has_final_cache)
    EnsureCurrentInteractiveTile(pdf_state, page_index, viewport);
  pdf_state->final_cache_pending = !has_final_cache;

  const float preview_source_width =
      std::max(1.0f, (float)pdf_state->current_preview.bitmap_width);
  const float preview_source_height =
      std::max(1.0f, (float)pdf_state->current_preview.bitmap_height);
  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height, kPdfPreviewPadding,
          kPdfPreviewPadding, kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
          kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->SetColorMode(0);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();

  if (has_final_cache) {
    const int crop_x = std::max(
        0, std::min(pdf_state->current_final_zoom.bitmap_width - 1,
                    (int)(viewport.left *
                          pdf_state->current_final_zoom.bitmap_width)));
    const int crop_y = std::max(
        0, std::min(pdf_state->current_final_zoom.bitmap_height - 1,
                    (int)(viewport.top *
                          pdf_state->current_final_zoom.bitmap_height)));
    int crop_w =
        std::max(1, (int)(viewport.width *
                              pdf_state->current_final_zoom.bitmap_width +
                          0.5f));
    int crop_h =
        std::max(1, (int)(viewport.height *
                              pdf_state->current_final_zoom.bitmap_height +
                          0.5f));
    if (crop_x + crop_w > pdf_state->current_final_zoom.bitmap_width)
      crop_w = pdf_state->current_final_zoom.bitmap_width - crop_x;
    if (crop_y + crop_h > pdf_state->current_final_zoom.bitmap_height)
      crop_h = pdf_state->current_final_zoom.bitmap_height - crop_y;

    BlitRgb565BitmapScaledCrop(
        ts, ts->screenleft, kPdfZoomScreenHeight, 0, 0, kPdfZoomScreenWidth,
        kPdfZoomScreenHeight, pdf_state->current_final_zoom.pixels,
        pdf_state->current_final_zoom.bitmap_width,
        pdf_state->current_final_zoom.bitmap_height, crop_x, crop_y, crop_w,
        crop_h);
  } else if (BitmapCacheCoversRect(pdf_state->current_interactive_tile,
                                   page_index, pdf_state->zoom_index,
                                   viewport)) {
    BlitRgb565BitmapScaledCrop(
        ts, ts->screenleft, kPdfZoomScreenHeight, 0, 0, kPdfZoomScreenWidth,
        kPdfZoomScreenHeight, pdf_state->current_interactive_tile.pixels,
        pdf_state->current_interactive_tile.bitmap_width,
        pdf_state->current_interactive_tile.bitmap_height, 0, 0,
        pdf_state->current_interactive_tile.bitmap_width,
        pdf_state->current_interactive_tile.bitmap_height);
  } else {
    ts->SetPen(18, 28);
    ts->PrintString("PDF render unavailable");
  }
  char top_msg[48];
  snprintf(top_msg, sizeof(top_msg), "PDF %u/%u  %.1fx",
           (unsigned)(page_index + 1), (unsigned)pdf_state->page_count,
           pdf_view_utils::ZoomForIndex(pdf_state->zoom_index));
  ts->SetPen(12, 18);
  ts->PrintString(top_msg);

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  if (app)
    app->DrawBottomGradientBackground();
  ts->FillRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kPdfPaper);

  if (!pdf_state->current_preview.pixels.empty() &&
      pdf_state->current_preview.bitmap_width > 0 &&
      pdf_state->current_preview.bitmap_height > 0) {
    BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, kPdfPreviewScreenHeight, preview_layout.x,
        preview_layout.y, preview_layout.width, preview_layout.height,
        pdf_state->current_preview.pixels,
        pdf_state->current_preview.bitmap_width,
        pdf_state->current_preview.bitmap_height, 0, 0,
        pdf_state->current_preview.bitmap_width,
        pdf_state->current_preview.bitmap_height);
  }
  ts->DrawRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kPdfFrame);

  const int viewport_x = preview_layout.x +
                         (int)(std::max(0.0f, viewport.left) * preview_layout.width + 0.5f);
  const int viewport_y = preview_layout.y +
                         (int)(std::max(0.0f, viewport.top) * preview_layout.height + 0.5f);
  const int viewport_w = std::max(
      1, (int)(std::min(1.0f, viewport.width) * preview_layout.width + 0.5f));
  const int viewport_h = std::max(
      1, (int)(std::min(1.0f, viewport.height) * preview_layout.height + 0.5f));
  ts->DrawRect((u16)viewport_x, (u16)viewport_y, (u16)(viewport_x + viewport_w),
               (u16)(viewport_y + viewport_h), kPdfAccent);

  ts->SetStyle(saved_style);
  ts->SetColorMode(saved_color);
  ts->SetScreen(saved_screen);
  ts->margin.bottom = saved_bottom_margin;
}

void Book::PrefetchAdjacentPdfPage() {
  if (!IsPdf() || !pdf_state)
    return;
  PrepareAdjacentPdfSlot(
      pdf_state, ClampPdfPageIndex(position, pdf_state->page_count), 1);
}

bool Book::PumpDeferredPdfWork(u32 budget_ms) {
  if (!IsPdf() || !pdf_state || !pdf_state->ctx || !pdf_state->doc)
    return false;

  const int page_index = ClampPdfPageIndex(position, pdf_state->page_count);
  const u64 start_ms = osGetTime();
  bool worked = false;

  if (pdf_state->final_cache_pending ||
      !BitmapCacheValid(pdf_state->current_final_zoom, page_index)) {
    if (EnsureCurrentFinalCache(pdf_state, page_index))
      worked = true;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (PrepareAdjacentPdfSlot(pdf_state, page_index, 1))
    worked = true;
  if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
    return worked;

  if (PrepareAdjacentPdfSlot(pdf_state, page_index, -1))
    worked = true;

  return worked;
}

uint8_t ParsePdfFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  const bool is_new_3ds = DetectNew3ds();
  const pdf_view_utils::DevicePolicy policy =
      pdf_view_utils::GetDevicePolicy(is_new_3ds);
  fz_context *ctx = fz_new_context(NULL, NULL, policy.mupdf_store_bytes);
  pdf_document *doc = NULL;
  fz_outline *outline = NULL;
  uint8_t rc = 0;
  int page_count = 0;

  if (!ctx)
    return BOOK_ERR_CORRUPT;

  fz_var(doc);
  fz_var(outline);
  fz_try(ctx) {
    doc = pdf_open_document(ctx, path);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open pdf document");
    if (pdf_needs_password(ctx, doc)) {
      rc = BOOK_ERR_PASSWORD;
    } else {
      page_count = fz_count_pages(ctx, (fz_document *)doc);
      if (page_count <= 0)
        rc = BOOK_ERR_CORRUPT;
      else
        outline = pdf_load_outline(ctx, doc);
    }
  }
  fz_catch(ctx) {
    if (book->GetApp())
      book->GetApp()->PrintStatus(fz_caught_message(ctx));
    if (rc == 0)
      rc = BOOK_ERR_CORRUPT;
  }

  if (rc != 0) {
    fz_drop_outline(ctx, outline);
    pdf_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return rc;
  }

  book->ClearChapters();
  book->ClearTocConfidence();
  PopulatePdfMetadata(book, ctx, doc);
  if (outline) {
    AddPdfOutlineEntries(book, ctx, doc, outline, 0);
    if (!book->GetChapters().empty()) {
      book->SetTocConfidence(TOC_QUALITY_STRONG,
                             (u16)std::min<size_t>(book->GetChapters().size(),
                                                   65535),
                             0, 0);
    }
  }
  book->InitPdfView((u16)std::min(page_count, 65535), ctx, doc, outline,
                    is_new_3ds);
  return 0;
}

uint8_t IndexPdfMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  pdf_document *doc = NULL;
  uint8_t rc = 0;

  if (!ctx)
    return BOOK_ERR_CORRUPT;

  fz_var(doc);
  fz_try(ctx) {
    doc = pdf_open_document(ctx, path);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open pdf document");
    if (pdf_needs_password(ctx, doc)) {
      rc = BOOK_ERR_PASSWORD;
    } else {
      PopulatePdfMetadata(book, ctx, doc);
    }
  }
  fz_catch(ctx) {
    if (rc == 0)
      rc = BOOK_ERR_CORRUPT;
  }

  pdf_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return rc;
}
