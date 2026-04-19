// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_worker.h"

#include "debug_log.h"
#include "formats/mupdf/mupdf_view.h"
#include "shared/debug_runtime_mode.h"


void CancelMuPdfIncrementalRenderState(Book::MuPdfState *mupdf_state) {
  if (!mupdf_state)
    return;
  if (mupdf_state->worker && mupdf_state->worker->job_submitted) {
    if (__atomic_load_n(&mupdf_state->worker->job_pending, __ATOMIC_ACQUIRE)) {
      LightEvent_Wait(&mupdf_state->worker->done_event);
    }
    LightEvent_Clear(&mupdf_state->worker->done_event);
    __atomic_store_n(&mupdf_state->worker->job_pending, false, __ATOMIC_RELEASE);
    mupdf_state->worker->job_submitted = false;
    mupdf_state->worker->job_strip_y0 = 0;
    mupdf_state->worker->job_strip_y1 = 0;
  }
  mupdf_state->incremental.active = false;
  mupdf_state->incremental.strips_completed = 0;
  mupdf_state->incremental.partial_pixels.clear();
  mupdf_state->incremental.partial_pixels.shrink_to_fit();
  mupdf_state->incremental.partial_width = 0;
  mupdf_state->incremental.partial_height = 0;
}

bool PromoteMuPdfAdjacentSlotIfMatching(Book::MuPdfState *mupdf_state,
                                          int page_index) {
  if (!mupdf_state)
    return false;
  Book::MuPdfState::AdjacentSlot *slot = NULL;
  if (mupdf_state->prev_slot.page == page_index)
    slot = &mupdf_state->prev_slot;
  else if (mupdf_state->next_slot.page == page_index)
    slot = &mupdf_state->next_slot;
  if (!slot)
    return false;

  ResetBitmapCache(&mupdf_state->current_preview);
  ResetBitmapCache(&mupdf_state->current_interactive_tile);
  mupdf_state->current_preview = slot->preview;
  mupdf_state->current_interactive_tile = slot->interactive_tile;
  ResetBitmapCache(&slot->preview);
  ResetBitmapCache(&slot->interactive_tile);

  if (mupdf_state->cached_display_list && mupdf_state->ctx)
    fz_drop_display_list(mupdf_state->ctx, mupdf_state->cached_display_list);
  mupdf_state->cached_display_list = slot->display_list;
  mupdf_state->cached_display_list_page = page_index;
  slot->display_list = NULL;
  slot->page = -1;
  mupdf_state->final_cache_pending =
      app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind);
  CancelMuPdfIncrementalRenderState(mupdf_state);
  return true;
}

bool EnsureMuPdfDisplayListForPage(Book::MuPdfState *mupdf_state,
                                        int page_index,
                                        fz_display_list **out_list) {
  if (!mupdf_state || !out_list)
    return false;
  if (mupdf_state->cached_display_list_page == page_index &&
      mupdf_state->cached_display_list) {
    *out_list = mupdf_state->cached_display_list;
    return true;
  }
  if (PromoteMuPdfAdjacentSlotIfMatching(mupdf_state, page_index) &&
      mupdf_state->cached_display_list_page == page_index &&
      mupdf_state->cached_display_list) {
    *out_list = mupdf_state->cached_display_list;
    return true;
  }

  if (mupdf_state->cached_display_list && mupdf_state->ctx) {
    fz_drop_display_list(mupdf_state->ctx, mupdf_state->cached_display_list);
    mupdf_state->cached_display_list = NULL;
    mupdf_state->cached_display_list_page = -1;
  }
  *out_list = NULL;
  return true;
}

bool EnsureCurrentMuPdfPreviewCache(Book::MuPdfState *mupdf_state, int page_index) {
  if (!mupdf_state)
    return false;
  DBG_LOGF_CAT(mupdf_state->reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
               "MUPDF preview: enter page=%d cached=%d current_page=%d",
               page_index,
               BitmapCacheValid(mupdf_state->current_preview, page_index) ? 1
                                                                          : 0,
               mupdf_state->current_preview.page);
  PromoteMuPdfAdjacentSlotIfMatching(mupdf_state, page_index);
  if (BitmapCacheValid(mupdf_state->current_preview, page_index))
    return true;

  fz_display_list *display_list = NULL;
  if (!EnsureMuPdfDisplayListForPage(mupdf_state, page_index, &display_list))
    return false;
  DBG_LOGF_CAT(mupdf_state->reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
               "MUPDF preview: display-list-ready page=%d have=%d cached_page=%d",
               page_index, display_list ? 1 : 0,
               mupdf_state->cached_display_list_page);

  RenderedMuPdfBitmap rendered;
  fz_display_list *new_list = NULL;
  float page_width = mupdf_state->page_width;
  float page_height = mupdf_state->page_height;
  const float preview_scale = ComputeMuPdfPreviewScale(page_width, page_height);
  DBG_LOGF_CAT(
      mupdf_state->reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
      "MUPDF preview: render-begin page=%d scale=%.4f page_size=(%.2f,%.2f)",
      page_index, (double)preview_scale, (double)page_width,
      (double)page_height);
  if (!RenderMuPdfBitmap(mupdf_state->ctx, mupdf_state->doc, page_index,
                       preview_scale,
                       &rendered, &page_width, &page_height, NULL,
                       display_list, display_list ? NULL : &new_list,
                       mupdf_state->reporter)) {
    DBG_LOGF_CAT(mupdf_state->reporter, DBG_LEVEL_WARN, DBG_CAT_RENDER,
                 "MUPDF preview: render-failed page=%d", page_index);
    return false;
  }
  DBG_LOGF_CAT(mupdf_state->reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
               "MUPDF preview: render-done page=%d bmp=%dx%d", page_index,
               rendered.width, rendered.height);

  float left = 0.0f, top = 0.0f, width = 1.0f, height = 1.0f;
  ComputeBitmapContentBoundsNormalized(rendered, &left, &top, &width, &height);
  StoreBitmapCache(&mupdf_state->current_preview, page_index, -1, left, top,
                   width, height, &rendered);
  mupdf_state->page_width = page_width;
  mupdf_state->page_height = page_height;
  if (new_list && !display_list) {
    mupdf_state->cached_display_list = new_list;
    mupdf_state->cached_display_list_page = page_index;
  }
  DBG_LOGF_CAT(
      mupdf_state->reporter, DBG_LEVEL_DEBUG, DBG_CAT_RENDER,
      "MUPDF preview: cache-store-done page=%d content=(%.3f,%.3f %.3f x %.3f)",
      page_index, (double)left, (double)top, (double)width, (double)height);
  return true;
}

bool EnsureCurrentMuPdfInteractiveTile(Book::MuPdfState *mupdf_state,
                                         int page_index) {
  if (!mupdf_state)
    return false;
  PromoteMuPdfAdjacentSlotIfMatching(mupdf_state, page_index);
  if (BitmapCacheValid(mupdf_state->current_interactive_tile, page_index))
    return true;

  fz_display_list *display_list = NULL;
  if (!EnsureMuPdfDisplayListForPage(mupdf_state, page_index, &display_list))
    return false;

  RenderedMuPdfBitmap rendered;
  fz_display_list *new_list = NULL;
  float page_width = mupdf_state->page_width;
  float page_height = mupdf_state->page_height;
  const bool render_ok = RenderMuPdfBitmap(mupdf_state->ctx, mupdf_state->doc, page_index,
                       kPdfInteractiveScale *
                           ComputeFitScale(page_width, page_height,
                                           kPdfZoomScreenWidth, kPdfZoomScreenHeight) *
                           ComputeEffectiveMuPdfZoom(
                               mupdf_state->document_kind,
                               mupdf_state->zoom_index),
                       &rendered, &page_width, &page_height, NULL,
                       display_list, display_list ? NULL : &new_list,
                       mupdf_state->reporter);
  if (!render_ok) {
    return false;
  }

  StoreBitmapCache(&mupdf_state->current_interactive_tile, page_index,
                   mupdf_state->zoom_index, 0.0f, 0.0f, 1.0f, 1.0f, &rendered);
  mupdf_state->page_width = page_width;
  mupdf_state->page_height = page_height;
  if (new_list && !display_list) {
    mupdf_state->cached_display_list = new_list;
    mupdf_state->cached_display_list_page = page_index;
  }
  return true;
}

static bool RenderMuPdfBitmapStrip(fz_context *ctx, fz_document *doc,
                                  int page_index, float scale,
                                  fz_display_list *reuse_list,
                                  int strip_y0_px, int strip_y1_px,
                                  int full_width_px, int full_height_px,
                                  std::vector<u16> *partial_pixels) {
  if (!ctx || !doc || !partial_pixels || !reuse_list)
    return false;
  if (strip_y0_px < 0 || strip_y1_px <= strip_y0_px ||
      strip_y1_px > full_height_px || full_width_px <= 0)
    return false;

  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  bool ok = false;

  fz_var(page);
  fz_var(pixmap);
  fz_var(device);

  fz_try(ctx) {
    page = fz_load_page(ctx, doc, page_index);
    fz_rect bounds = fz_bound_page(ctx, page);
    if (!(bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0))
      fz_throw(ctx, FZ_ERROR_FORMAT, "empty pdf page bounds");

    const fz_matrix ctm = MakeMuPdfRenderMatrix(bounds, scale);

    const fz_irect bbox = fz_make_irect(0, strip_y0_px, full_width_px, strip_y1_px);
    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pixmap, 255);
    device = fz_new_draw_device(ctx, fz_identity, pixmap);

    const fz_rect clip = fz_make_rect(0.0f, (float)strip_y0_px,
                                       (float)full_width_px, (float)strip_y1_px);
    fz_run_display_list(ctx, reuse_list, device, ctm, clip, NULL);
    fz_close_device(ctx, device);

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);
    if (!samples || pix_w <= 0 || pix_h <= 0 || comps < 1)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid strip pixmap");

    EnsureGrayLut();
    const size_t row_offset = (size_t)strip_y0_px * (size_t)full_width_px;
    u16 *dst = partial_pixels->data() + row_offset;
    if (comps == 1) {
      for (int y = 0; y < pix_h; y++) {
        const unsigned char *src = samples + (size_t)y * (size_t)stride;
        for (int x = 0; x < pix_w; x++)
          *dst++ = g_gray_to_rgb565[*src++];
      }
    } else {
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
  fz_catch(ctx) { ok = false; }
  return ok;
}

static void MuPdfWorkerThreadFunc(void *arg) {
  Book::MuPdfState *mupdf_state = static_cast<Book::MuPdfState *>(arg);
  Book::MuPdfState::MuPdfWorker *w = mupdf_state->worker;

  while (true) {
    LightEvent_Wait(&w->submit_event);
    LightEvent_Clear(&w->submit_event);

    if (__atomic_load_n(&w->shutdown_requested, __ATOMIC_ACQUIRE))
      break;

    if (!__atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
      continue;

    w->job_result = RenderMuPdfBitmapStrip(
        w->worker_ctx, mupdf_state->doc,
        w->job_page_index, w->job_scale, w->job_display_list,
        w->job_strip_y0, w->job_strip_y1,
        w->job_full_width, w->job_full_height,
        w->job_pixel_buf);

    __atomic_store_n(&w->job_pending, false, __ATOMIC_RELEASE);
    LightEvent_Signal(&w->done_event);
  }
}

void InitMuPdfWorker(Book::MuPdfState *mupdf_state) {
  if (debug_runtime::ForceSynchronousMuPdfRender() ||
      (mupdf_state &&
       !app_flow_utils::MuPdfWantsFinalQualityRender(
           mupdf_state->document_kind) &&
       !app_flow_utils::MuPdfShouldPrefetchAdjacent(
           mupdf_state->document_kind))) {
    if (mupdf_state)
      mupdf_state->worker_init_attempted = true;
    return;
  }
  if (!mupdf_state || !mupdf_state->is_new_3ds || !mupdf_state->ctx)
    return;
  mupdf_state->worker_init_attempted = true;

  Book::MuPdfState::MuPdfWorker *w = new Book::MuPdfState::MuPdfWorker();

  w->worker_ctx = fz_clone_context(mupdf_state->ctx);
  if (!w->worker_ctx) {
    delete w;
    return;
  }

  fz_set_aa_level(w->worker_ctx, kMuPdfAaLevel);

  LightEvent_Init(&w->submit_event, RESET_STICKY);
  LightEvent_Init(&w->done_event,   RESET_STICKY);

  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

  mupdf_state->worker = w;
  w->thread_handle = threadCreate(MuPdfWorkerThreadFunc, mupdf_state,
                                   32 * 1024, prio + 1, 1, false);
  if (!w->thread_handle) {
    fz_drop_context(w->worker_ctx);
    w->worker_ctx = NULL;
    delete w;
    mupdf_state->worker = NULL;
    return;
  }
}

void ShutdownMuPdfWorker(Book::MuPdfState *mupdf_state) {
  if (!mupdf_state || !mupdf_state->worker)
    return;

  Book::MuPdfState::MuPdfWorker *w = mupdf_state->worker;

  __atomic_store_n(&w->shutdown_requested, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&w->submit_event);

  if (w->thread_handle) {
    threadJoin(w->thread_handle, U64_MAX);
    threadFree(w->thread_handle);
    w->thread_handle = NULL;
  }

  if (w->worker_ctx) {
    fz_drop_context(w->worker_ctx);
    w->worker_ctx = NULL;
  }

  delete w;
  mupdf_state->worker = NULL;
}

bool PumpMuPdfIncrementalStripWorker(Book::MuPdfState *mupdf_state,
                                        int page_index) {
  Book::MuPdfState::IncrementalRenderState &inc = mupdf_state->incremental;
  Book::MuPdfState::MuPdfWorker *w = mupdf_state->worker;

  if (__atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE)) {
    return false;
  }

  bool strip_just_collected = false;

  if (w->job_submitted) {
    LightEvent_Wait(&w->done_event);
    LightEvent_Clear(&w->done_event);
    w->job_submitted = false;

    const bool ok = w->job_result;

    if (!ok) {
      CancelMuPdfIncrementalRenderState(mupdf_state);
      mupdf_state->final_cache_pending =
          app_flow_utils::MuPdfWantsFinalQualityRender(
              mupdf_state->document_kind);
      return false;
    }

    inc.strips_completed++;
    strip_just_collected = true;

    if (inc.strips_completed >= inc.strips_total) {
      RenderedMuPdfBitmap promoted;
      promoted.width  = inc.partial_width;
      promoted.height = inc.partial_height;
      promoted.pixels.swap(inc.partial_pixels);
      StoreBitmapCache(&mupdf_state->current_final_zoom, page_index,
                       mupdf_state->max_zoom_index, 0.0f, 0.0f, 1.0f, 1.0f,
                       &promoted);
      mupdf_state->final_cache_pending = false;
      CancelMuPdfIncrementalRenderState(mupdf_state);
      return true;
    }

    // Expose the freshly completed strip to the UI before the next worker
    // job starts. Submitting immediately races with DrawCurrentView on
    // inc.partial_pixels and can leave later bands visually blank.
    return strip_just_collected;
  }

  fz_display_list *display_list = NULL;
  if (!EnsureMuPdfDisplayListForPage(mupdf_state, page_index, &display_list) ||
      !display_list)
    return false;

  const int s = inc.strips_completed;
  const float scale = ComputeMuPdfFinalScale(mupdf_state->document_kind,
                                             mupdf_state->page_width,
                                             mupdf_state->page_height,
                                             mupdf_state->max_zoom_index);
  const int y0 = (s * inc.partial_height) / inc.strips_total;
  const int y1 = (s == inc.strips_total - 1)
                     ? inc.partial_height
                     : ((s + 1) * inc.partial_height) / inc.strips_total;

  w->job_page_index   = page_index;
  w->job_scale        = scale;
  w->job_display_list = display_list;
  w->job_strip_y0     = y0;
  w->job_strip_y1     = y1;
  w->job_full_width   = inc.partial_width;
  w->job_full_height  = inc.partial_height;
  w->job_pixel_buf    = &inc.partial_pixels;
  w->job_result       = false;
  w->job_submitted    = true;

  LightEvent_Clear(&w->done_event);
  __atomic_store_n(&w->job_pending, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&w->submit_event);

  return strip_just_collected;
}

bool PumpMuPdfIncrementalStrip(Book::MuPdfState *mupdf_state, int page_index) {
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return false;
  if (!app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind)) {
    mupdf_state->final_cache_pending = false;
    CancelMuPdfIncrementalRenderState(mupdf_state);
    return false;
  }

  Book::MuPdfState::IncrementalRenderState &inc = mupdf_state->incremental;

  if (BitmapCacheValid(mupdf_state->current_final_zoom, page_index) &&
      mupdf_state->current_final_zoom.zoom_index >= mupdf_state->max_zoom_index) {
    mupdf_state->final_cache_pending = false;
    CancelMuPdfIncrementalRenderState(mupdf_state);
    return false;
  }

  if (!inc.active ||
      inc.target_page != page_index ||
      inc.target_zoom_index != mupdf_state->max_zoom_index) {
    CancelMuPdfIncrementalRenderState(mupdf_state);

    fz_display_list *display_list = NULL;
    if (!EnsureMuPdfDisplayListForPage(mupdf_state, page_index, &display_list) ||
        !display_list)
      return false;

    const float scale = ComputeMuPdfFinalScale(mupdf_state->document_kind,
                                               mupdf_state->page_width,
                                               mupdf_state->page_height,
                                               mupdf_state->max_zoom_index);
    fz_page *pg = NULL;
    fz_rect bounds = fz_empty_rect;
    fz_var(pg);
    bool measured = false;
    fz_try(mupdf_state->ctx) {
      pg = fz_load_page(mupdf_state->ctx, (fz_document *)mupdf_state->doc, page_index);
      bounds = fz_bound_page(mupdf_state->ctx, pg);
      measured = (bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0);
    }
    fz_always(mupdf_state->ctx) { fz_drop_page(mupdf_state->ctx, pg); }
    fz_catch(mupdf_state->ctx) { measured = false; }
    if (!measured)
      return false;

    const fz_matrix ctm = MakeMuPdfRenderMatrix(bounds, scale);
    const fz_irect full_bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
    const int full_w = full_bbox.x1 - full_bbox.x0;
    const int full_h = full_bbox.y1 - full_bbox.y0;
    if (full_w <= 0 || full_h <= 0)
      return false;

    inc.active = true;
    inc.target_page = page_index;
    inc.target_zoom_index = mupdf_state->max_zoom_index;
    inc.strips_completed = 0;
    inc.strips_total = mupdf_state->is_new_3ds ? kPdfStripsNew3DS : kPdfStripsOld3DS;
    inc.partial_width = full_w;
    inc.partial_height = full_h;
    inc.partial_pixels.assign((size_t)full_w * (size_t)full_h, kPdfPaper);

  }

  // Lazy-init worker for N3DS in case this mupdf_state was reused without
  // going through InitPdfView again (e.g. ReuseParsedBook path).
  if (!mupdf_state->worker && !mupdf_state->worker_init_attempted && mupdf_state->is_new_3ds) {
    mupdf_state->worker_init_attempted = true;
    InitMuPdfWorker(mupdf_state);
  }

  if (mupdf_state->worker)
    return PumpMuPdfIncrementalStripWorker(mupdf_state, page_index);

  fz_display_list *display_list = NULL;
  if (!EnsureMuPdfDisplayListForPage(mupdf_state, page_index, &display_list) ||
      !display_list)
    return false;

  const int s = inc.strips_completed;
  const float scale = ComputeMuPdfFinalScale(mupdf_state->document_kind,
                                             mupdf_state->page_width,
                                             mupdf_state->page_height,
                                             mupdf_state->max_zoom_index);

  const int strip_y0 = (s * inc.partial_height) / inc.strips_total;
  const int strip_y1 = (s == inc.strips_total - 1)
                           ? inc.partial_height
                           : ((s + 1) * inc.partial_height) / inc.strips_total;

  const bool ok = RenderMuPdfBitmapStrip(mupdf_state->ctx, mupdf_state->doc,
                                        page_index, scale, display_list,
                                        strip_y0, strip_y1,
                                        inc.partial_width, inc.partial_height,
                                        &inc.partial_pixels);
  if (ok) {
    inc.strips_completed++;
  } else {
    CancelMuPdfIncrementalRenderState(mupdf_state);
    mupdf_state->final_cache_pending =
        app_flow_utils::MuPdfWantsFinalQualityRender(
            mupdf_state->document_kind);
    return false;
  }

  if (inc.strips_completed >= inc.strips_total) {
    RenderedMuPdfBitmap promoted;
    promoted.width = inc.partial_width;
    promoted.height = inc.partial_height;
    promoted.pixels.swap(inc.partial_pixels);
    StoreBitmapCache(&mupdf_state->current_final_zoom, page_index,
                     mupdf_state->max_zoom_index, 0.0f, 0.0f, 1.0f, 1.0f,
                     &promoted);
    mupdf_state->final_cache_pending = false;
    CancelMuPdfIncrementalRenderState(mupdf_state);
    return true;
  }

  return ok;
}


static Book::MuPdfState::AdjacentSlot *GetAdjacentSlot(Book::MuPdfState *mupdf_state,
                                                     int direction) {
  if (!mupdf_state)
    return NULL;
  return (direction < 0) ? &mupdf_state->prev_slot : &mupdf_state->next_slot;
}

bool PrepareAdjacentMuPdfSlot(Book::MuPdfState *mupdf_state, int current_page,
                                   int direction) {
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc || direction == 0)
    return false;
  const int page_index =
      ClampMuPdfPageIndex(current_page + (direction < 0 ? -1 : 1),
                        mupdf_state->page_count);
  if (page_index == current_page)
    return false;

  Book::MuPdfState::AdjacentSlot *slot = GetAdjacentSlot(mupdf_state, direction);
  if (!slot)
    return false;

  if (slot->page == page_index &&
      BitmapCacheValid(slot->preview, page_index) &&
      BitmapCacheValid(slot->interactive_tile, page_index)) {
    return false;
  }

  ResetAdjacentSlot(slot, mupdf_state->ctx);

  float page_width = mupdf_state->page_width;
  float page_height = mupdf_state->page_height;
  RenderedMuPdfBitmap preview;
  fz_display_list *display_list = NULL;
  if (!RenderMuPdfBitmap(mupdf_state->ctx, mupdf_state->doc, page_index,
                       ComputeMuPdfPreviewScale(page_width, page_height),
                       &preview, &page_width, &page_height, NULL, NULL,
                       &display_list, mupdf_state->reporter) ||
      !display_list) {
    ResetAdjacentSlot(slot, mupdf_state->ctx);
    return false;
  }

  float left = 0.0f, top = 0.0f, width = 1.0f, height = 1.0f;
  ComputeBitmapContentBoundsNormalized(preview, &left, &top, &width, &height);
  StoreBitmapCache(&slot->preview, page_index, -1, left, top, width, height,
                   &preview);

  RenderedMuPdfBitmap interactive;
  {
    const bool render_ok = RenderMuPdfBitmap(mupdf_state->ctx, mupdf_state->doc, page_index,
                         kPdfInteractiveScale *
                             ComputeFitScale(page_width, page_height,
                                             kPdfZoomScreenWidth, kPdfZoomScreenHeight) *
                             ComputeEffectiveMuPdfZoom(
                                 mupdf_state->document_kind,
                                 mupdf_state->zoom_index),
                         &interactive, &page_width, &page_height, NULL,
                         display_list, NULL, mupdf_state->reporter);
    if (!render_ok) {
      ResetAdjacentSlot(slot, mupdf_state->ctx);
      return false;
    }
  }
  StoreBitmapCache(&slot->interactive_tile, page_index, mupdf_state->zoom_index,
                   0.0f, 0.0f, 1.0f, 1.0f, &interactive);

  slot->page = page_index;
  slot->display_list = display_list;
  return true;
}
