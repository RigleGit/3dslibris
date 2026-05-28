// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_render.h"

#include <algorithm>

#include "shared/debug_log.h"
#include "formats/mupdf/mupdf_render_policy_utils.h"

extern "C" {
#include "mupdf/fitz/buffer.h"
#include "mupdf/pdf.h"
}

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

static size_t CountPdfContentBytes(fz_context *ctx, pdf_obj *obj,
                                   size_t byte_cap) {
  if (!ctx || !obj || byte_cap == 0)
    return 0;

  if (pdf_is_array(ctx, obj)) {
    size_t total = 0;
    const int len = pdf_array_len(ctx, obj);
    for (int i = 0; i < len && total <= byte_cap; i++) {
      total += CountPdfContentBytes(ctx, pdf_array_get(ctx, obj, i),
                                    byte_cap - std::min(total, byte_cap));
    }
    return total;
  }

  if (!pdf_is_stream(ctx, obj))
    return 0;

  fz_buffer *buffer = NULL;
  size_t size = 0;
  fz_var(buffer);
  fz_try(ctx) {
    buffer = pdf_load_raw_stream(ctx, obj);
    size = fz_buffer_storage(ctx, buffer, NULL);
  }
  fz_always(ctx) { fz_drop_buffer(ctx, buffer); }
  fz_catch(ctx) { size = byte_cap + 1; }
  return size;
}

bool EstimateMuPdfPageRenderComplexity(fz_context *ctx, fz_document *doc,
                                        int page_index, int *xobject_count,
                                        size_t *content_bytes) {
  if (xobject_count)
    *xobject_count = 0;
  if (content_bytes)
    *content_bytes = 0;
  if (!ctx || !doc)
    return false;

  pdf_page *page = NULL;
  bool ok = false;
  fz_var(page);
  fz_try(ctx) {
    pdf_document *pdf = pdf_document_from_fz_document(ctx, doc);
    if (!pdf)
      fz_throw(ctx, FZ_ERROR_FORMAT, "not a pdf document");
    page = pdf_load_page(ctx, pdf, page_index);
    pdf_obj *resources = pdf_page_resources(ctx, page);
    pdf_obj *xobjects =
        resources ? pdf_dict_get(ctx, resources, PDF_NAME(XObject)) : NULL;
    if (xobject_count)
      *xobject_count = xobjects ? pdf_dict_len(ctx, xobjects) : 0;

    if (content_bytes) {
      pdf_obj *contents = pdf_page_contents(ctx, page);
      *content_bytes = CountPdfContentBytes(
          ctx, contents,
          mupdf_render_policy_utils::kOld3dsPdfPreviewMaxContentBytes + 1);
    }
    ok = true;
  }
  fz_always(ctx) { pdf_drop_page(ctx, page); }
  fz_catch(ctx) {
    if (xobject_count)
      *xobject_count = 0;
    if (content_bytes)
      *content_bytes =
          mupdf_render_policy_utils::kOld3dsPdfPreviewMaxContentBytes + 1;
    ok = false;
  }
  return ok;
}

bool RenderMuPdfBitmap(fz_context *ctx, fz_document *doc, int page_index,
                       float scale, RenderedMuPdfBitmap *out,
                       float *page_width, float *page_height,
                       const pdf_view_utils::NormalizedRect *crop_rect,
                       fz_display_list *reuse_list,
                       fz_display_list **out_list,
                       IStatusReporter *reporter) {
  if (!ctx || !doc || !out || scale <= 0.0f)
    return false;
  DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
               "MUPDF render: begin page=%d scale=%.4f reuse_list=%d crop=%d",
               page_index, (double)scale, reuse_list ? 1 : 0,
               crop_rect ? 1 : 0);

  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  fz_display_list *list = reuse_list;
  fz_rect bounds = fz_empty_rect;
  fz_rect render_rect = fz_empty_rect;
  fz_irect bbox = fz_empty_irect;
  bool ok = false;
  bool owns_list = false;
  const bool direct_page_render = (reuse_list == NULL && out_list == NULL);

  fz_var(page);
  fz_var(pixmap);
  fz_var(device);
  fz_var(list);

  fz_try(ctx) {
    DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                 "MUPDF render: load-page-begin page=%d", page_index);
    page = fz_load_page(ctx, doc, page_index);
    bounds = fz_bound_page(ctx, page);
    if (!(bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0))
      fz_throw(ctx, FZ_ERROR_FORMAT, "empty pdf page bounds");

    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                 "MUPDF render: load-page-done page=%d bounds=(%.2f,%.2f)",
                 page_index, (double)width, (double)height);
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

    if (!list && !direct_page_render) {
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: display-list-build-begin page=%d",
                   page_index);
      list = fz_new_display_list_from_page(ctx, page);
      owns_list = true;
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: display-list-build-done page=%d",
                   page_index);
    }

    const fz_matrix ctm = MakeMuPdfRenderMatrix(bounds, scale);
    bbox = fz_round_rect(fz_transform_rect(render_rect, ctm));
    if (bbox.x1 <= bbox.x0 || bbox.y1 <= bbox.y0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf render bbox");

    // Grayscale rendering: 3x less memory bandwidth for MuPDF's rasterizer.
    // Manga pages are B&W, so visual quality is identical.
    DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                 "MUPDF render: pixmap-begin page=%d bbox=%d,%d %dx%d",
                 page_index, bbox.x0, bbox.y0, bbox.x1 - bbox.x0,
                 bbox.y1 - bbox.y0);
    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pixmap, 255);
    device = fz_new_draw_device(ctx, fz_identity, pixmap);
    if (direct_page_render) {
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: run-page-begin page=%d", page_index);
      fz_run_page(ctx, page, device, ctm, NULL);
      fz_close_device(ctx, device);
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: run-page-done page=%d", page_index);
    } else {
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: run-list-begin page=%d", page_index);
      fz_run_display_list(ctx, list, device, ctm, fz_infinite_rect, NULL);
      fz_close_device(ctx, device);
      DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                   "MUPDF render: run-list-done page=%d", page_index);
    }

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
    DBG_LOGF_CAT(
        reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
        "MUPDF render: convert-begin page=%d pix=%dx%d comps=%d stride=%d",
        page_index, pix_w, pix_h, comps, stride);
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
    DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
                 "MUPDF render: convert-done page=%d pix=%dx%d", page_index,
                 pix_w, pix_h);
    ok = true;
  }
  fz_always(ctx) {
    fz_drop_device(ctx, device);
    fz_drop_pixmap(ctx, pixmap);
    fz_drop_page(ctx, page);
  }
  fz_catch(ctx) {
    DBG_LOGF_CAT(reporter, DBG_LEVEL_WARN, DBG_CAT_RENDER,
                 "MUPDF render error: %s", fz_caught_message(ctx));
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

  DBG_LOGF_CAT(reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
               "MUPDF render: end page=%d ok=%d", page_index, ok ? 1 : 0);

  return ok;
}

float ComputeMuPdfPreviewScale(float page_width, float page_height) {
  return ComputeFitScale(page_width, page_height,
                         kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
                         kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
}

float ComputeMuPdfFinalScale(app_flow_utils::MuPdfDocumentKind document_kind,
                             float page_width, float page_height,
                             int max_zoom_index) {
  return ComputeFitScale(page_width, page_height, kPdfZoomScreenWidth,
                         kPdfZoomScreenHeight) *
         ComputeEffectiveMuPdfZoom(document_kind, max_zoom_index);
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
  char subject_buf[512];
  char created_buf[512];
  title_buf[0] = '\0';
  author_buf[0] = '\0';
  subject_buf[0] = '\0';
  created_buf[0] = '\0';

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
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_SUBJECT, subject_buf,
                           sizeof(subject_buf)) > 0 &&
        subject_buf[0]) {
      book->SetSubjects(subject_buf);
    }
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_CREATIONDATE, created_buf,
                           sizeof(created_buf)) > 0 &&
        created_buf[0]) {
      book->SetPublished(created_buf);
    }
  }
  fz_catch(ctx) {
  }
}
