// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_render.h"

#include <algorithm>

static void ComputeBitmapContentBoundsPx(const RenderedMuPdfBitmap &bitmap,
                                         MuPdfBitmapBoundsPx *bounds) {
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

void ComputeBitmapContentBoundsNormalized(const RenderedMuPdfBitmap &bitmap,
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

  MuPdfBitmapBoundsPx bounds;
  ComputeBitmapContentBoundsPx(bitmap, &bounds);
  *left = (float)bounds.left / (float)bitmap.width;
  *top = (float)bounds.top / (float)bitmap.height;
  *width = std::max(0.0001f, (float)bounds.width / (float)bitmap.width);
  *height = std::max(0.0001f, (float)bounds.height / (float)bitmap.height);
}

fz_matrix MakeMuPdfRenderMatrix(fz_rect page_bounds, float scale) {
  return fz_transform_page(page_bounds, 72.0f * scale, 0.0f);
}

bool QueryMuPdfPageMetrics(fz_context *ctx, fz_document *doc,
                                int page_index, float *page_width,
                                float *page_height) {
  if (!ctx || !doc || !page_width || !page_height)
    return false;

  fz_page *page = NULL;
  fz_rect bounds = fz_empty_rect;
  bool ok = false;

  fz_var(page);
  fz_try(ctx) {
    page = fz_load_page(ctx, doc, page_index);
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

bool RenderMuPdfBitmap(fz_context *ctx, fz_document *doc, int page_index,
                       float scale, RenderedMuPdfBitmap *out,
                       float *page_width, float *page_height,
                       const pdf_view_utils::NormalizedRect *crop_rect,
                       fz_display_list *reuse_list,
                       fz_display_list **out_list) {
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
    page = fz_load_page(ctx, doc, page_index);
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

    const fz_matrix ctm = MakeMuPdfRenderMatrix(bounds, scale);
    bbox = fz_round_rect(fz_transform_rect(render_rect, ctm));
    if (bbox.x1 <= bbox.x0 || bbox.y1 <= bbox.y0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf render bbox");

    // Grayscale rendering: 3x less memory bandwidth for MuPDF's rasterizer.
    // Manga pages are B&W, so visual quality is identical.
    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
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
          *dst++ = g_gray_to_rgb565[*src++];
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

float ComputeMuPdfPreviewScale(float page_width, float page_height) {
  return ComputeFitScale(page_width, page_height,
                         kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
                         kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
}

float ComputeMuPdfFinalScale(float page_width, float page_height,
                                  int max_zoom_index) {
  return ComputeFitScale(page_width, page_height, kPdfZoomScreenWidth,
                         kPdfZoomScreenHeight) *
         ComputeEffectiveMuPdfZoom(max_zoom_index);
}
void AddMuPdfOutlineEntries(Book *book, fz_context *ctx, fz_document *doc,
                                 const fz_outline *entry, u8 level) {
  if (!book || !ctx || !doc)
    return;

  for (const fz_outline *node = entry; node; node = node->next) {
    int page_index = -1;
    fz_try(ctx) {
      if (node->page.chapter >= 0 && node->page.page >= 0) {
        page_index = fz_page_number_from_location(ctx, doc, node->page);
      } else if (node->uri && node->uri[0]) {
        float x = 0.0f;
        float y = 0.0f;
        fz_location loc = fz_resolve_link(ctx, doc, node->uri, &x, &y);
        page_index = fz_page_number_from_location(ctx, doc, loc);
      }
    }
    fz_catch(ctx) { page_index = -1; }

    if (page_index >= 0 && page_index <= 65535 && node->title &&
        node->title[0]) {
      book->AddChapter((u16)page_index, node->title,
                       (u8)std::min<int>(level, 7));
    }

    if (node->down)
      AddMuPdfOutlineEntries(book, ctx, doc, node->down, (u8)(level + 1));
  }
}

void PopulateMuPdfMetadata(Book *book, fz_context *ctx, fz_document *doc) {
  if (!book || !ctx || !doc)
    return;

  char title_buf[512];
  char author_buf[512];
  title_buf[0] = '\0';
  author_buf[0] = '\0';

  fz_try(ctx) {
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, title_buf,
                           sizeof(title_buf)) > 0 &&
        title_buf[0]) {
      book->SetTitle(title_buf);
    }
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_AUTHOR, author_buf,
                           sizeof(author_buf)) > 0 &&
        author_buf[0]) {
      std::string author(author_buf);
      book->SetAuthor(author);
    }
  }
  fz_catch(ctx) {
  }
}
