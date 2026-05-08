// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/pdf/pdf.h"

#include "book/book.h"
#include "book/cover_layout_constants.h"
#include "formats/mupdf/mupdf_document.h"
#include "formats/mupdf/mupdf_render.h"
#include "formats/mupdf/mupdf_render_policy_utils.h"
#include "shared/app_flow_utils.h"

#include <algorithm>
#include <vector>

namespace {

static bool AssignCoverFromRgb565(Book *book, const uint16_t *src, int src_w,
                                  int src_h) {
  if (!book || !src || src_w <= 0 || src_h <= 0)
    return false;

  float scale_x =
      (float)src_w / (float)cover_layout::kBrowserCoverThumbWidth;
  float scale_y =
      (float)src_h / (float)cover_layout::kBrowserCoverThumbHeight;
  float scale = std::max(scale_x, scale_y);
  if (scale < 1.0f)
    scale = 1.0f;

  const int dst_w =
      std::max(1, std::min(cover_layout::kBrowserCoverThumbWidth,
                           (int)(src_w / scale)));
  const int dst_h =
      std::max(1, std::min(cover_layout::kBrowserCoverThumbHeight,
                           (int)(src_h / scale)));

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = NULL;
  }
  book->coverPixels = new u16[(size_t)dst_w * (size_t)dst_h];
  if (!book->coverPixels)
    return false;

  book->coverWidth = dst_w;
  book->coverHeight = dst_h;
  for (int y = 0; y < dst_h; y++) {
    int src_y = std::min(src_h - 1, (int)(y * scale));
    for (int x = 0; x < dst_w; x++) {
      int src_x = std::min(src_w - 1, (int)(x * scale));
      book->coverPixels[(size_t)y * (size_t)dst_w + (size_t)x] =
          src[(size_t)src_y * (size_t)src_w + (size_t)src_x];
    }
  }
  return true;
}

} // namespace

uint8_t ParsePdfFile(Book *book, const char *path) {
  return ParseMuPdfFile(book, path);
}

uint8_t IndexPdfMetadata(Book *book, const char *path) {
  return IndexMuPdfMetadata(book, path);
}

int pdf_extract_cover(Book *book, const std::string &pdfpath) {
  if (!book || pdfpath.empty())
    return 1;

  InitMuPdfLocks();
  fz_context *ctx = fz_new_context(NULL, &g_mupdf_locks_ctx,
                                   FZ_STORE_DEFAULT);
  fz_document *doc = NULL;
  int rc = 0;

  if (!ctx)
    return 2;

  fz_set_aa_level(ctx, kMuPdfAaLevel);

  fz_var(doc);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, pdfpath.c_str());
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open document");
    if (fz_needs_password(ctx, doc))
      rc = 3;
  }
  fz_catch(ctx) {
    if (rc == 0)
      rc = 4;
  }

  if (rc != 0) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return rc;
  }

  float page_width = 0.0f;
  float page_height = 0.0f;
  if (!QueryMuPdfPageMetrics(ctx, doc, 0, &page_width, &page_height) ||
      page_width <= 0.0f || page_height <= 0.0f) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 5;
  }

  int xobject_count = 0;
  size_t content_bytes = 0;
  if (EstimateMuPdfPageRenderComplexity(ctx, doc, 0, &xobject_count,
                                        &content_bytes) &&
      mupdf_render_policy_utils::ShouldSkipPdfPageRender(
          app_flow_utils::MuPdfDocumentKind::Pdf, xobject_count,
          content_bytes)) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 8;
  }

  const float fit_scale =
      std::min((float)cover_layout::kBrowserCoverThumbWidth / page_width,
               (float)cover_layout::kBrowserCoverThumbHeight / page_height);
  const float render_scale = std::max(0.25f, fit_scale * 2.0f);
  RenderedMuPdfBitmap rendered;
  if (!RenderMuPdfBitmap(ctx, doc, 0, render_scale, &rendered, NULL, NULL,
                         NULL, NULL, NULL) ||
      rendered.width <= 0 || rendered.height <= 0 || rendered.pixels.empty()) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 6;
  }

  const bool ok = AssignCoverFromRgb565(book, rendered.pixels.data(),
                                        rendered.width, rendered.height);
  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return ok ? 0 : 7;
}
