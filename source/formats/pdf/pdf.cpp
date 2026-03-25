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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "mupdf/fitz/color.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/device.h"
#include "mupdf/fitz/document.h"
#include "mupdf/fitz/geometry.h"
#include "mupdf/fitz/outline.h"
#include "mupdf/fitz/pixmap.h"
#include "mupdf/pdf/document.h"
}

namespace {

static const int kPdfPreviewScreenWidth = 240;
static const int kPdfPreviewScreenHeight = 320;
static const int kPdfZoomScreenWidth = 240;
static const int kPdfZoomScreenHeight = 400;
static const u16 kPdfPaper = 0xFFFF;
static const u16 kPdfFrame = 0x2104;
static const u16 kPdfAccent = 0x0000;

struct RenderedPdfBitmap {
  int width;
  int height;
  std::vector<u16> pixels;

  RenderedPdfBitmap() : width(0), height(0), pixels() {}
};

static u16 RGB565FromRgb8(unsigned char r, unsigned char g, unsigned char b) {
  return (u16)(((u16)(r >> 3) << 11) | ((u16)(g >> 2) << 5) | (u16)(b >> 3));
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

static int ClampPdfPageIndex(int page_index, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (page_index < 0)
    return 0;
  if (page_index >= (int)page_count)
    return (int)page_count - 1;
  return page_index;
}

static void BlitRgb565Bitmap(Text *ts, u16 *screen, int logical_height, int x,
                             int y, const std::vector<u16> &pixels, int width,
                             int height) {
  if (!ts || !screen || pixels.empty() || width <= 0 || height <= 0)
    return;
  const int stride = ts->display.height;
  const int logical_width = ts->display.width;
  ts->MarkScreenDirty(screen);
  for (int row = 0; row < height; row++) {
    const int dy = y + row;
    if (dy < 0 || dy >= logical_height)
      continue;
    const u16 *src = &pixels[(size_t)row * (size_t)width];
    for (int col = 0; col < width; col++) {
      const int dx = x + col;
      if (dx < 0 || dx >= logical_width)
        continue;
      screen[(size_t)dy * (size_t)stride + (size_t)dx] = src[col];
    }
  }
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
                            float scale,
                            const pdf_view_utils::NormalizedRect *crop_rect,
                            RenderedPdfBitmap *out, float *page_width,
                            float *page_height) {
  if (!ctx || !doc || !out || scale <= 0.0f)
    return false;

  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  fz_rect bounds = fz_empty_rect;
  fz_rect render_rect = fz_empty_rect;
  fz_irect bbox = fz_empty_irect;
  bool ok = false;

  fz_var(page);
  fz_var(pixmap);
  fz_var(device);

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
      const float left = std::max(0.0f, std::min(1.0f, crop_rect->left));
      const float top = std::max(0.0f, std::min(1.0f, crop_rect->top));
      const float right =
          std::max(left, std::min(1.0f, crop_rect->left + crop_rect->width));
      const float bottom =
          std::max(top, std::min(1.0f, crop_rect->top + crop_rect->height));
      render_rect.x0 = bounds.x0 + left * width;
      render_rect.y0 = bounds.y0 + top * height;
      render_rect.x1 = bounds.x0 + right * width;
      render_rect.y1 = bounds.y0 + bottom * height;
    }

    fz_matrix ctm = fz_transform_page(bounds, 72.0f * scale, 0.0f);
    bbox = fz_round_rect(fz_transform_rect(render_rect, ctm));
    if (bbox.x1 <= bbox.x0 || bbox.y1 <= bbox.y0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf render bbox");

    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pixmap, 255);
    device = fz_new_draw_device(ctx, ctm, pixmap);
    fz_run_page(ctx, page, device, ctm, NULL);
    fz_close_device(ctx, device);

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);
    if (!samples || pix_w <= 0 || pix_h <= 0 || stride <= 0 || comps < 3)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid pdf pixmap");

    out->width = pix_w;
    out->height = pix_h;
    out->pixels.assign((size_t)pix_w * (size_t)pix_h, kPdfPaper);
    for (int y = 0; y < pix_h; y++) {
      const unsigned char *src = samples + (size_t)y * (size_t)stride;
      for (int x = 0; x < pix_w; x++) {
        const unsigned char *p = src + x * comps;
        out->pixels[(size_t)y * (size_t)pix_w + (size_t)x] =
            RGB565FromRgb8(p[0], p[1], p[2]);
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
    ok = false;
  }

  return ok;
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
    fz_drop_outline(pdf_state->ctx, pdf_state->outline);
    pdf_drop_document(pdf_state->ctx, pdf_state->doc);
    fz_drop_context(pdf_state->ctx);
  }
  delete pdf_state;
  pdf_state = NULL;
}

void Book::InitPdfView(u16 page_count, fz_context *ctx, pdf_document *doc,
                       fz_outline *outline) {
  ResetPdfState();
  pdf_state = new PdfState();
  pdf_state->ctx = ctx;
  pdf_state->doc = doc;
  pdf_state->outline = outline;
  pdf_state->page_count = page_count;
  pdf_state->zoom_index = pdf_view_utils::DefaultZoomIndex();
  pdf_state->viewport_center_x = 0.5f;
  pdf_state->viewport_center_y = 0.5f;
}

bool Book::ChangePdfZoom(int delta) {
  if (!IsPdf() || !pdf_state || delta == 0)
    return false;
  const int next =
      pdf_view_utils::ClampZoomIndex(pdf_state->zoom_index + delta);
  if (next == pdf_state->zoom_index)
    return false;
  pdf_state->zoom_index = next;
  return true;
}

bool Book::MovePdfViewportToPreview(int touch_x, int touch_y) {
  if (!IsPdf() || !pdf_state)
    return false;
  const pdf_view_utils::PreviewLayout preview = pdf_view_utils::ComputePreviewLayout(
      pdf_state->page_width, pdf_state->page_height, kPdfPreviewScreenWidth,
      kPdfPreviewScreenHeight);
  const pdf_view_utils::NormalizedRect viewport =
      pdf_view_utils::ComputeViewportRect(
          pdf_state->page_width, pdf_state->page_height,
          pdf_view_utils::ZoomForIndex(pdf_state->zoom_index),
          (float)kPdfZoomScreenWidth, (float)kPdfZoomScreenHeight,
          pdf_state->viewport_center_x, pdf_state->viewport_center_y);
  const pdf_view_utils::NormalizedPoint center =
      pdf_view_utils::RecenterViewportFromPreview(preview, viewport, touch_x,
                                                  touch_y);
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

  pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayout(
          pdf_state->page_width, pdf_state->page_height, kPdfPreviewScreenWidth,
          kPdfPreviewScreenHeight);
  pdf_view_utils::NormalizedRect viewport = pdf_view_utils::ComputeViewportRect(
      pdf_state->page_width, pdf_state->page_height,
      pdf_view_utils::ZoomForIndex(pdf_state->zoom_index),
      (float)kPdfZoomScreenWidth, (float)kPdfZoomScreenHeight,
      pdf_state->viewport_center_x, pdf_state->viewport_center_y);
  pdf_state->viewport_center_x = viewport.left + viewport.width * 0.5f;
  pdf_state->viewport_center_y = viewport.top + viewport.height * 0.5f;

  const float preview_scale =
      ComputeFitScale(pdf_state->page_width, pdf_state->page_height,
                      preview_layout.width, preview_layout.height);
  const float tile_scale =
      ComputeFitScale(pdf_state->page_width, pdf_state->page_height,
                      kPdfZoomScreenWidth, kPdfZoomScreenHeight) *
      pdf_view_utils::ZoomForIndex(pdf_state->zoom_index);

  if (pdf_state->cached_preview_page != page_index ||
      pdf_state->cached_preview_width != preview_layout.width ||
      pdf_state->cached_preview_height != preview_layout.height) {
    RenderedPdfBitmap rendered;
    if (RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index,
                        preview_scale, NULL, &rendered, &page_width,
                        &page_height)) {
      pdf_state->cached_preview_page = page_index;
      pdf_state->cached_preview_width = rendered.width;
      pdf_state->cached_preview_height = rendered.height;
      pdf_state->cached_preview_pixels.swap(rendered.pixels);
      pdf_state->page_width = page_width;
      pdf_state->page_height = page_height;
    }
  }

  if (pdf_state->cached_tile_page != page_index ||
      pdf_state->cached_tile_zoom_index != pdf_state->zoom_index ||
      pdf_state->cached_tile_center_x != pdf_state->viewport_center_x ||
      pdf_state->cached_tile_center_y != pdf_state->viewport_center_y) {
    RenderedPdfBitmap rendered;
    if (RenderPdfBitmap(pdf_state->ctx, pdf_state->doc, page_index, tile_scale,
                        &viewport, &rendered, &page_width, &page_height)) {
      pdf_state->cached_tile_page = page_index;
      pdf_state->cached_tile_zoom_index = pdf_state->zoom_index;
      pdf_state->cached_tile_center_x = pdf_state->viewport_center_x;
      pdf_state->cached_tile_center_y = pdf_state->viewport_center_y;
      pdf_state->cached_tile_width = rendered.width;
      pdf_state->cached_tile_height = rendered.height;
      pdf_state->cached_tile_pixels.swap(rendered.pixels);
      pdf_state->page_width = page_width;
      pdf_state->page_height = page_height;
    }
  }

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->SetColorMode(0);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  const int tile_draw_x =
      std::max(0, (kPdfZoomScreenWidth - pdf_state->cached_tile_width) / 2);
  const int tile_draw_y =
      std::max(0, (kPdfZoomScreenHeight - pdf_state->cached_tile_height) / 2);
  if (!pdf_state->cached_tile_pixels.empty()) {
    BlitRgb565Bitmap(ts, ts->screenleft, kPdfZoomScreenHeight, tile_draw_x,
                     tile_draw_y, pdf_state->cached_tile_pixels,
                     pdf_state->cached_tile_width,
                     pdf_state->cached_tile_height);
    ts->DrawRect((u16)tile_draw_x, (u16)tile_draw_y,
                 (u16)(tile_draw_x + pdf_state->cached_tile_width),
                 (u16)(tile_draw_y + pdf_state->cached_tile_height), kPdfFrame);
  } else {
    ts->SetPen(18, 28);
    ts->PrintString("PDF render unavailable");
  }
  char top_msg[64];
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

  const int preview_draw_x = preview_layout.x +
                             std::max(0, preview_layout.width -
                                             pdf_state->cached_preview_width) /
                                 2;
  const int preview_draw_y = preview_layout.y +
                             std::max(0, preview_layout.height -
                                             pdf_state->cached_preview_height) /
                                 2;
  if (!pdf_state->cached_preview_pixels.empty()) {
    BlitRgb565Bitmap(ts, ts->screenright, kPdfPreviewScreenHeight,
                     preview_draw_x, preview_draw_y,
                     pdf_state->cached_preview_pixels,
                     pdf_state->cached_preview_width,
                     pdf_state->cached_preview_height);
  }
  ts->DrawRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kPdfFrame);

  const int viewport_x = preview_draw_x +
                         (int)(viewport.left * pdf_state->cached_preview_width +
                               0.5f);
  const int viewport_y = preview_draw_y +
                         (int)(viewport.top * pdf_state->cached_preview_height +
                               0.5f);
  const int viewport_w =
      std::max(1, (int)(viewport.width * pdf_state->cached_preview_width + 0.5f));
  const int viewport_h = std::max(
      1, (int)(viewport.height * pdf_state->cached_preview_height + 0.5f));
  ts->DrawRect((u16)viewport_x, (u16)viewport_y, (u16)(viewport_x + viewport_w),
               (u16)(viewport_y + viewport_h), kPdfAccent);

  ts->SetPen(8, 20);
  ts->PrintString("preview");

  ts->SetStyle(saved_style);
  ts->SetColorMode(saved_color);
  ts->SetScreen(saved_screen);
  ts->margin.bottom = saved_bottom_margin;
}

uint8_t ParsePdfFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
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
  book->InitPdfView((u16)std::min(page_count, 65535), ctx, doc, outline);
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
