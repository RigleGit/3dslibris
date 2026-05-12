// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_document.h"

#include "book/book.h"
#include "formats/common/book_error.h"
#include "formats/mupdf/mupdf_render.h"
#include "formats/mupdf/mupdf_worker.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/open_cancel_poll.h"
#include "shared/path_utils.h"
#include "shared/status_reporter.h"

#include "shared/debug_log.h"

#include <dirent.h>
#include <string.h>

// Scan the app font directory once for a CJK-compatible font and cache the
// result. Returns the sdmc: path, or an empty string if none is found.
static const std::string &FindUserCjkFontPath() {
  static std::string s_path;
  static bool s_scanned = false;
  if (s_scanned)
    return s_path;
  s_scanned = true;
  const std::string &font_dir = paths::GetFontDir();
  DIR *dp = opendir(font_dir.c_str());
  if (!dp)
    return s_path;
  struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    const char *name = ent->d_name;
    for (int i = 0; i < paths::kCjkFontPatternCount; i++) {
      if (strstr(name, paths::kCjkFontPatterns[i])) {
        s_path = font_dir + "/" + name;
        closedir(dp);
        return s_path;
      }
    }
  }
  closedir(dp);
  return s_path;
}

static fz_font *MuPdfLoadCjkFont(fz_context *ctx, const char * /*name*/,
                                  int /*ordering*/, int /*serif*/) {
  const std::string &path = FindUserCjkFontPath();
  if (path.empty())
    return NULL;
  fz_font *font = NULL;
  fz_try(ctx) { font = fz_new_font_from_file(ctx, NULL, path.c_str(), 0, 0); }
  fz_catch(ctx) { font = NULL; }
  return font;
}

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
  mupdf_state->page_width_cache.assign(page_count, 0.0f);
  mupdf_state->page_height_cache.assign(page_count, 0.0f);
  mupdf_state->page_metrics_valid.assign(page_count, 0);
  mupdf_state->is_new_3ds = is_new_3ds;
  mupdf_state->document_kind = document_kind;
  mupdf_state->reporter = GetStatusReporter();
  mupdf_state->keep_preview_cache = policy.keep_preview_cache;
  mupdf_state->keep_tile_cache = policy.keep_tile_cache;
  mupdf_state->viewport.max_zoom_index = policy.max_zoom_index;
  mupdf_state->viewport.zoom_index = policy.default_zoom_index;
  mupdf_state->viewport.center_x = 0.5f;
  mupdf_state->viewport.center_y = 0.5f;
  mupdf_state->final_cache_pending =
      app_flow_utils::MuPdfWantsFinalQualityRender(document_kind);
  InitMuPdfWorker(mupdf_state);
}

uint8_t ParseMuPdfFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  // Yield to APT before starting MuPDF — handles HOME pressed just before open.
  if (open_cancel_poll::Poll(book, book->GetStatusReporter(), "pdf-pre"))
    return BOOK_ERR_CANCELLED;

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
  fz_install_load_system_font_funcs(ctx, NULL, MuPdfLoadCjkFont, NULL);

  // Step 1: open the document.
  fz_var(doc);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "unable to open document");
    if (fz_needs_password(ctx, doc))
      rc = BOOK_ERR_PASSWORD;
  }
  fz_catch(ctx) {
    IStatusReporter *reporter = book->GetStatusReporter();
    if (reporter)
      reporter->PrintStatus(fz_caught_message(ctx));
    if (rc == 0)
      rc = BOOK_ERR_CORRUPT;
  }

  if (rc != 0) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return rc;
  }

  // Yield between steps — handles HOME pressed during fz_open_document.
  if (open_cancel_poll::Poll(book, book->GetStatusReporter(), "pdf-open")) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return BOOK_ERR_CANCELLED;
  }

  // Step 2: count pages.
  fz_try(ctx) {
    page_count = fz_count_pages(ctx, doc);
    if (page_count <= 0)
      rc = BOOK_ERR_CORRUPT;
  }
  fz_catch(ctx) {
    if (rc == 0)
      rc = BOOK_ERR_CORRUPT;
  }

  if (rc != 0) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return rc;
  }

  // Yield before loading the outline (TOC can be large in some PDFs).
  if (open_cancel_poll::Poll(book, book->GetStatusReporter(), "pdf-pages")) {
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return BOOK_ERR_CANCELLED;
  }

  // Step 3: load outline (non-fatal if it fails).
  fz_var(outline);
  fz_try(ctx) {
    outline = fz_load_outline(ctx, doc);
  }
  fz_catch(ctx) {
    outline = NULL;
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

  fz_context *ctx = fz_new_context(NULL, NULL, 4u * 1024u * 1024u);
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
