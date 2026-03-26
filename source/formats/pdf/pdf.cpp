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
                            fz_display_list *reuse_list = NULL,
                            fz_display_list **out_list = NULL) {
  if (!ctx || !doc || !out || scale <= 0.0f)
    return false;

  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  fz_display_list *list = reuse_list;
  fz_rect bounds = fz_empty_rect;
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

    // Build display list if not provided (captures page ops for cheap replay).
    if (!list) {
      list = fz_new_display_list_from_page(ctx, page);
      owns_list = true;
    }

    const fz_matrix ctm = MakePdfRenderMatrix(bounds, scale);
    bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
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
    if (pdf_state->prefetch_display_list)
      fz_drop_display_list(pdf_state->ctx, pdf_state->prefetch_display_list);
    if (pdf_state->cached_display_list)
      fz_drop_display_list(pdf_state->ctx, pdf_state->cached_display_list);
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
  return true;
}

bool Book::MovePdfViewportToPreview(int touch_x, int touch_y) {
  if (!IsPdf() || !pdf_state)
    return false;
  float preview_source_width = pdf_state->page_width;
  float preview_source_height = pdf_state->page_height;
  if (pdf_state->cached_preview_width > 0 && pdf_state->cached_preview_height > 0) {
    preview_source_width = (float)pdf_state->cached_preview_width;
    preview_source_height = (float)pdf_state->cached_preview_height;
  }
  const pdf_view_utils::PreviewLayout preview =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height,
          kPdfPreviewPadding, kPdfPreviewPadding,
          kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
          kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
  const PdfNavigationBounds nav = GetPdfNavigationBounds(
      pdf_state->cached_preview_content_left,
      pdf_state->cached_preview_content_top,
      pdf_state->cached_preview_content_width,
      pdf_state->cached_preview_content_height);
  const pdf_view_utils::NormalizedRect viewport = ComputePdfViewportRect(
      pdf_state->page_width, pdf_state->page_height, pdf_state->zoom_index,
      pdf_state->viewport_center_x, pdf_state->viewport_center_y,
      pdf_state->cached_preview_content_left,
      pdf_state->cached_preview_content_top,
      pdf_state->cached_preview_content_width,
      pdf_state->cached_preview_content_height);
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

  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;
  if (QueryPdfPageMetrics(pdf_state->ctx, pdf_state->doc, page_index,
                          &page_width, &page_height)) {
    pdf_state->page_width = page_width;
    pdf_state->page_height = page_height;
  }

  pdf_view_utils::NormalizedRect viewport = pdf_view_utils::ComputeViewportRect(
      pdf_state->page_width, pdf_state->page_height,
      ComputeEffectivePdfZoom(pdf_state->zoom_index),
      (float)kPdfZoomScreenWidth, (float)kPdfZoomScreenHeight,
      pdf_state->viewport_center_x, pdf_state->viewport_center_y);
  pdf_state->viewport_center_x = viewport.left + viewport.width * 0.5f;
  pdf_state->viewport_center_y = viewport.top + viewport.height * 0.5f;

  // Low-res preview: only needs to fit the bottom screen preview box
  // (~232x312 px) instead of the old full-res approach (~1080x1577 px).
  const float preview_scale =
      ComputeFitScale(pdf_state->page_width, pdf_state->page_height,
                      kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
                      kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);

  // Manage display list cache: parse page content once, replay cheaply.
  fz_display_list *display_list = NULL;
  if (pdf_state->cached_display_list_page == page_index) {
    display_list = pdf_state->cached_display_list;
  } else {
    // Drop old display list.
    if (pdf_state->cached_display_list && pdf_state->ctx) {
      fz_drop_display_list(pdf_state->ctx, pdf_state->cached_display_list);
      pdf_state->cached_display_list = NULL;
      pdf_state->cached_display_list_page = -1;
    }
    // Check if the prefetch has a display list for this page.
    if (pdf_state->prefetch_display_list &&
        pdf_state->prefetch_page == page_index) {
      display_list = pdf_state->prefetch_display_list;
      pdf_state->cached_display_list = display_list;
      pdf_state->cached_display_list_page = page_index;
      pdf_state->prefetch_display_list = NULL; // ownership transferred
    }
  }

  if (pdf_state->cached_preview_page != page_index ||
      pdf_state->cached_preview_width <= 0 ||
      pdf_state->cached_preview_height <= 0) {
    // Check prefetch cache first.
    if (pdf_state->prefetch_page == page_index &&
        pdf_state->prefetch_preview_width > 0 &&
        !pdf_state->prefetch_preview_pixels.empty()) {
      pdf_state->cached_preview_page = page_index;
      pdf_state->cached_preview_width = pdf_state->prefetch_preview_width;
      pdf_state->cached_preview_height = pdf_state->prefetch_preview_height;
      pdf_state->cached_preview_pixels.swap(pdf_state->prefetch_preview_pixels);
      pdf_state->cached_preview_content_left = pdf_state->prefetch_content_left;
      pdf_state->cached_preview_content_top = pdf_state->prefetch_content_top;
      pdf_state->cached_preview_content_width = pdf_state->prefetch_content_width;
      pdf_state->cached_preview_content_height = pdf_state->prefetch_content_height;
    } else {
      RenderedPdfBitmap rendered;
      fz_display_list *new_list = NULL;
      if (RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                          preview_scale, &rendered, &page_width,
                          &page_height, display_list,
                          display_list ? NULL : &new_list)) {
        ComputeBitmapContentBoundsNormalized(
            rendered, &pdf_state->cached_preview_content_left,
            &pdf_state->cached_preview_content_top,
            &pdf_state->cached_preview_content_width,
            &pdf_state->cached_preview_content_height);
        pdf_state->cached_preview_page = page_index;
        pdf_state->cached_preview_width = rendered.width;
        pdf_state->cached_preview_height = rendered.height;
        pdf_state->cached_preview_pixels.swap(rendered.pixels);
        pdf_state->page_width = page_width;
        pdf_state->page_height = page_height;

        // Capture newly created display list for reuse.
        if (new_list && !display_list) {
          display_list = new_list;
          pdf_state->cached_display_list = display_list;
          pdf_state->cached_display_list_page = page_index;
        }
      }
    }
  }

  // Zoom cache: render once at the highest zoom level for this device.
  // Lower zoom levels just crop a bigger region and downscale (looks great).
  // Only re-render if the user zooms ABOVE the cached resolution.
  const float tile_scale =
      ComputeFitScale(pdf_state->page_width, pdf_state->page_height,
                      kPdfZoomScreenWidth, kPdfZoomScreenHeight) *
      ComputeEffectivePdfZoom(pdf_state->max_zoom_index);

  const bool zoom_cache_invalid =
      pdf_state->cached_zoom_page != page_index ||
      pdf_state->cached_zoom_width <= 0 ||
      pdf_state->zoom_index > pdf_state->cached_zoom_index;

  if (zoom_cache_invalid) {
    // Check prefetch cache — accept it if page matches and resolution is
    // sufficient (i.e. prefetched at >= current zoom index).
    bool used_prefetch = false;
    if (pdf_state->prefetch_page == page_index &&
        pdf_state->prefetch_zoom_index >= pdf_state->zoom_index &&
        pdf_state->prefetch_width > 0 &&
        !pdf_state->prefetch_zoom_pixels.empty()) {
      pdf_state->cached_zoom_page = page_index;
      pdf_state->cached_zoom_index = pdf_state->prefetch_zoom_index;
      pdf_state->cached_zoom_width = pdf_state->prefetch_width;
      pdf_state->cached_zoom_height = pdf_state->prefetch_height;
      pdf_state->cached_zoom_pixels.swap(pdf_state->prefetch_zoom_pixels);
      used_prefetch = true;
    }
    if (!used_prefetch) {
      RenderedPdfBitmap rendered;
      fz_display_list *new_list = NULL;
      if (RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                          tile_scale, &rendered, &page_width, &page_height,
                          display_list,
                          display_list ? NULL : &new_list)) {
        pdf_state->cached_zoom_page = page_index;
        pdf_state->cached_zoom_index = pdf_state->max_zoom_index;
        pdf_state->cached_zoom_width = rendered.width;
        pdf_state->cached_zoom_height = rendered.height;
        pdf_state->cached_zoom_pixels.swap(rendered.pixels);
        pdf_state->page_width = page_width;
        pdf_state->page_height = page_height;

        if (new_list && !display_list) {
          display_list = new_list;
          pdf_state->cached_display_list = display_list;
          pdf_state->cached_display_list_page = page_index;
        }
      }
    }
  }

  const float preview_source_width =
      std::max(1.0f, (float)pdf_state->cached_preview_width);
  const float preview_source_height =
      std::max(1.0f, (float)pdf_state->cached_preview_height);
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

  // Extract the visible viewport crop from the full-page zoom cache.
  if (!pdf_state->cached_zoom_pixels.empty() &&
      pdf_state->cached_zoom_width > 0 && pdf_state->cached_zoom_height > 0) {
    const int crop_x = std::max(0, std::min(pdf_state->cached_zoom_width - 1,
        (int)(viewport.left * pdf_state->cached_zoom_width)));
    const int crop_y = std::max(0, std::min(pdf_state->cached_zoom_height - 1,
        (int)(viewport.top * pdf_state->cached_zoom_height)));
    int crop_w = std::max(1, (int)(viewport.width * pdf_state->cached_zoom_width + 0.5f));
    int crop_h = std::max(1, (int)(viewport.height * pdf_state->cached_zoom_height + 0.5f));
    // Clamp to stay within the cached bitmap.
    if (crop_x + crop_w > pdf_state->cached_zoom_width)
      crop_w = pdf_state->cached_zoom_width - crop_x;
    if (crop_y + crop_h > pdf_state->cached_zoom_height)
      crop_h = pdf_state->cached_zoom_height - crop_y;

    // Destination: fit the crop into the top screen, centered.
    const int draw_w = std::min(kPdfZoomScreenWidth, crop_w);
    const int draw_h = std::min(kPdfZoomScreenHeight, crop_h);
    const int draw_x = std::max(0, (kPdfZoomScreenWidth - draw_w) / 2);
    const int draw_y = std::max(0, (kPdfZoomScreenHeight - draw_h) / 2);

    const int stride = ts->display.height;
    const int logical_width = ts->display.width;
    ts->MarkScreenDirty(ts->screenleft);
    for (int row = 0; row < draw_h; row++) {
      const int dy = draw_y + row;
      if (dy < 0 || dy >= kPdfZoomScreenHeight)
        continue;
      const int src_y = crop_y + std::min(crop_h - 1, (row * crop_h) / draw_h);
      for (int col = 0; col < draw_w; col++) {
        const int dx = draw_x + col;
        if (dx < 0 || dx >= logical_width)
          continue;
        const int src_x = crop_x + std::min(crop_w - 1, (col * crop_w) / draw_w);
        ts->screenleft[(size_t)dy * (size_t)stride + (size_t)dx] =
            pdf_state->cached_zoom_pixels[(size_t)src_y *
                (size_t)pdf_state->cached_zoom_width + (size_t)src_x];
      }
    }

    ts->DrawRect((u16)draw_x, (u16)draw_y,
                 (u16)(draw_x + draw_w), (u16)(draw_y + draw_h), kPdfFrame);
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

  if (!pdf_state->cached_preview_pixels.empty() &&
      pdf_state->cached_preview_width > 0 &&
      pdf_state->cached_preview_height > 0) {
    BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, kPdfPreviewScreenHeight, preview_layout.x,
        preview_layout.y, preview_layout.width, preview_layout.height,
        pdf_state->cached_preview_pixels, pdf_state->cached_preview_width,
        pdf_state->cached_preview_height, 0, 0,
        pdf_state->cached_preview_width, pdf_state->cached_preview_height);
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

  if (!pdf_state->keep_preview_cache) {
    pdf_state->cached_preview_page = -1;
    pdf_state->cached_preview_width = 0;
    pdf_state->cached_preview_height = 0;
    pdf_state->cached_preview_pixels.clear();
  }
  // The zoom page cache is always kept — it is the mechanism that enables
  // smooth viewport panning without re-invoking MuPDF.  It is naturally
  // invalidated when the page or zoom level changes.
}

void Book::PrefetchAdjacentPdfPage() {
  if (!IsPdf() || !pdf_state || !pdf_state->ctx || !pdf_state->doc)
    return;

  const int current = ClampPdfPageIndex(position, pdf_state->page_count);
  const int next = current + 1;
  if (next >= (int)pdf_state->page_count)
    return;

  const int target_zoom_index = pdf_state->max_zoom_index;

  // Already prefetched this page at the highest cached zoom.
  if (pdf_state->prefetch_page == next &&
      pdf_state->prefetch_zoom_index == target_zoom_index &&
      pdf_state->prefetch_width > 0)
    return;

  // Invalidate old prefetch.
  if (pdf_state->prefetch_display_list && pdf_state->ctx) {
    fz_drop_display_list(pdf_state->ctx, pdf_state->prefetch_display_list);
    pdf_state->prefetch_display_list = NULL;
  }
  pdf_state->prefetch_page = -1;
  pdf_state->prefetch_zoom_pixels.clear();
  pdf_state->prefetch_preview_pixels.clear();

  float page_width = pdf_state->page_width;
  float page_height = pdf_state->page_height;

  // 1. Render low-res preview (creates display list as side-effect).
  const float preview_scale =
      ComputeFitScale(page_width, page_height,
                      kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
                      kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);

  fz_display_list *list = NULL;
  RenderedPdfBitmap preview;
  if (!RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, next,
                       preview_scale, &preview, &page_width, &page_height,
                       NULL, &list))
    return;

  float cl = 0.0f, ct = 0.0f, cw = 1.0f, ch = 1.0f;
  ComputeBitmapContentBoundsNormalized(preview, &cl, &ct, &cw, &ch);
  pdf_state->prefetch_content_left = cl;
  pdf_state->prefetch_content_top = ct;
  pdf_state->prefetch_content_width = cw;
  pdf_state->prefetch_content_height = ch;
  pdf_state->prefetch_preview_width = preview.width;
  pdf_state->prefetch_preview_height = preview.height;
  pdf_state->prefetch_preview_pixels.swap(preview.pixels);
  pdf_state->prefetch_display_list = list;

  // 2. Render zoom cache using the display list (replay only, no re-parse).
  const float tile_scale =
      ComputeFitScale(page_width, page_height,
                      kPdfZoomScreenWidth, kPdfZoomScreenHeight) *
      ComputeEffectivePdfZoom(target_zoom_index);

  RenderedPdfBitmap zoom;
  if (RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, next,
                      tile_scale, &zoom, &page_width, &page_height,
                      list, NULL)) {
    pdf_state->prefetch_page = next;
    pdf_state->prefetch_zoom_index = target_zoom_index;
    pdf_state->prefetch_width = zoom.width;
    pdf_state->prefetch_height = zoom.height;
    pdf_state->prefetch_zoom_pixels.swap(zoom.pixels);
  }
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
