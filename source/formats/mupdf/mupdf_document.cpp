// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_document.h"

#include "book/book.h"
#include "app/app.h"
#include "formats/common/book_error.h"
#include "formats/mupdf/mupdf_render.h"
#include "formats/mupdf/mupdf_worker.h"
#include "shared/pdf_view_utils.h"

#include "debug_log.h"

void Book::ResetMuPdfState() {
  if (!mupdf_state)
    return;
  ShutdownMuPdfWorker(mupdf_state);
  if (mupdf_state->ctx) {
    if (mupdf_state->cached_display_list)
      fz_drop_display_list(mupdf_state->ctx, mupdf_state->cached_display_list);
    ResetAdjacentSlot(&mupdf_state->prev_slot, mupdf_state->ctx);
    ResetAdjacentSlot(&mupdf_state->next_slot, mupdf_state->ctx);
    fz_drop_outline(mupdf_state->ctx, mupdf_state->outline);
    fz_drop_document(mupdf_state->ctx, mupdf_state->doc);
    fz_drop_context(mupdf_state->ctx);
  }
  delete mupdf_state;
  mupdf_state = NULL;
}

void Book::InitMuPdfView(u16 page_count, fz_context *ctx, fz_document *doc,
                       fz_outline *outline, bool is_new_3ds,
                       app_flow_utils::MuPdfDocumentKind document_kind) {
  ResetMuPdfState();
  mupdf_state = new MuPdfState();
  const pdf_view_utils::DevicePolicy policy =
      pdf_view_utils::GetDevicePolicy(is_new_3ds);
  mupdf_state->ctx = ctx;
  mupdf_state->doc = doc;
  mupdf_state->outline = outline;
  mupdf_state->page_count = page_count;
  mupdf_state->is_new_3ds = is_new_3ds;
  mupdf_state->document_kind = document_kind;
  mupdf_state->keep_preview_cache = policy.keep_preview_cache;
  mupdf_state->keep_tile_cache = policy.keep_tile_cache;
  mupdf_state->max_zoom_index = policy.max_zoom_index;
  mupdf_state->zoom_index = policy.default_zoom_index;
  mupdf_state->viewport_center_x = 0.5f;
  mupdf_state->viewport_center_y = 0.5f;
  mupdf_state->final_cache_pending =
      app_flow_utils::MuPdfWantsFinalQualityRender(document_kind);
  InitMuPdfWorker(mupdf_state);
}

uint8_t ParseMuPdfFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  const bool is_new_3ds = DetectNew3ds();
  const app_flow_utils::MuPdfDocumentKind document_kind =
      app_flow_utils::DetectMuPdfDocumentKind(path);
  const pdf_view_utils::DevicePolicy policy =
      pdf_view_utils::GetDevicePolicy(is_new_3ds);
  InitMuPdfLocks();
  fz_context *ctx =
      fz_new_context(NULL, &g_mupdf_locks_ctx, policy.mupdf_store_bytes);
  fz_document *doc = NULL;
  fz_outline *outline = NULL;
  uint8_t rc = 0;
  int page_count = 0;

  if (!ctx)
    return BOOK_ERR_CORRUPT;

  fz_set_aa_level(ctx, kMuPdfAaLevel);

  fz_var(doc);
  fz_var(outline);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open document");
    if (fz_needs_password(ctx, doc)) {
      rc = BOOK_ERR_PASSWORD;
    } else {
      page_count = fz_count_pages(ctx, doc);
      if (page_count <= 0)
        rc = BOOK_ERR_CORRUPT;
      else
        outline = fz_load_outline(ctx, doc);
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
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return rc;
  }

  book->ClearChapters();
  book->ClearTocConfidence();
  PopulateMuPdfMetadata(book, ctx, doc);
  if (outline) {
    AddMuPdfOutlineEntries(book, ctx, doc, outline, 0);
    if (!book->GetChapters().empty()) {
      book->SetTocConfidence(TOC_QUALITY_STRONG,
                             (u16)std::min<size_t>(book->GetChapters().size(),
                                                   65535),
                             0, 0);
    }
  }
  book->InitMuPdfView((u16)std::min(page_count, 65535), ctx, doc, outline,
                    is_new_3ds, document_kind);
  return 0;
}

uint8_t IndexMuPdfMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  fz_document *doc = NULL;
  uint8_t rc = 0;

  if (!ctx)
    return BOOK_ERR_CORRUPT;

  fz_set_aa_level(ctx, kMuPdfAaLevel);

  fz_var(doc);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open document");
    if (fz_needs_password(ctx, doc)) {
      rc = BOOK_ERR_PASSWORD;
    } else {
      PopulateMuPdfMetadata(book, ctx, doc);
    }
  }
  fz_catch(ctx) {
    if (rc == 0)
      rc = BOOK_ERR_CORRUPT;
  }

  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return rc;
}
