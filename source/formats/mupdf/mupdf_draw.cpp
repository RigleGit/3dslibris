#include "shared/fixed_layout_perf.h"
// SPDX-License-Identifier: AGPL-3.0-or-later
// MuPDF draw dispatch: screen blit helpers, deferred work pump, DrawCurrentMuPdfView.
// Viewport state management is in mupdf_viewport.cpp.

#include "formats/mupdf/mupdf_view.h"

#include "book/page.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "settings/prefs.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "ui/text.h"

namespace {

static void EnsureMuPdfPageMetrics(Book::MuPdfState *mupdf_state,
                                   int page_index) {
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return;
  if (page_index < 0 || page_index >= (int)mupdf_state->page_count)
    return;

  if ((size_t)page_index < mupdf_state->page_metrics_valid.size() &&
      mupdf_state->page_metrics_valid[page_index]) {
    mupdf_state->page_width = mupdf_state->page_width_cache[page_index];
    mupdf_state->page_height = mupdf_state->page_height_cache[page_index];
    return;
  }

  float page_width = mupdf_state->page_width;
  float page_height = mupdf_state->page_height;
  if (!QueryMuPdfPageMetrics(mupdf_state->ctx, mupdf_state->doc, page_index,
                             &page_width, &page_height)) {
    return;
  }

  mupdf_state->page_width = page_width;
  mupdf_state->page_height = page_height;
  if ((size_t)page_index < mupdf_state->page_metrics_valid.size()) {
    mupdf_state->page_width_cache[page_index] = page_width;
    mupdf_state->page_height_cache[page_index] = page_height;
    mupdf_state->page_metrics_valid[page_index] = 1;
  }
}

static void DrawMuPdfLoadFailure(Book *book, Text *ts, int page_index,
                                  bool complex_page) {
  if (!book || !ts)
    return;

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  if (complex_page) {
    ts->SetPen(14, 28);
    ts->PrintString("Page too complex");
    ts->SetPen(14, 52);
    ts->PrintString("to render on 3DS.");
    ts->SetPen(14, 84);
    char page_msg[48];
    snprintf(page_msg, sizeof(page_msg), "page %d", page_index + 1);
    ts->PrintString(page_msg);
  } else {
    ts->SetPen(14, 28);
    ts->PrintString("PDF page unavailable");
    ts->SetPen(14, 52);
    char page_msg[48];
    snprintf(page_msg, sizeof(page_msg), "page %d", page_index + 1);
    ts->PrintString(page_msg);
  }

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  book->DrawBottomGradientBackground();
  if (complex_page) {
    ts->SetPen(12, 22);
    ts->PrintString("Page too complex.");
    ts->SetPen(12, 40);
    ts->PrintString("On your computer:");
    ts->SetPen(12, 58);
    ts->PrintString("simplify the PDF,");
    ts->SetPen(12, 76);
    ts->PrintString("or convert to CBZ");
    ts->SetPen(12, 94);
    ts->PrintString("with JPG images.");
    ts->SetPen(12, 122);
    ts->PrintString("START: back to library");
  } else {
    ts->SetPen(12, 28);
    ts->PrintString("MuPDF preview failed");
    ts->SetPen(12, 48);
    ts->PrintString("use L/R or B to leave");
  }

  ts->SetStyle(saved_style);
  ts->SetColorMode(saved_color);
  ts->SetScreen(saved_screen);
  ts->margin.bottom = saved_bottom_margin;
}

} // namespace

int ClampMuPdfPageIndex(int page_index, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (page_index < 0)
    return 0;
  if (page_index >= (int)page_count)
    return (int)page_count - 1;
  return page_index;
}

MuPdfDeferredStage GetNextMuPdfDeferredStage(
    const Book::MuPdfState *mupdf_state, int page_index,
    const pdf_view_utils::NormalizedRect &viewport) {
  (void)viewport;
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return MuPdfDeferredStage::None;
  if (debug_runtime::ForceSynchronousMuPdfRender())
    return MuPdfDeferredStage::None;

  if (!BitmapCacheValid(mupdf_state->current_preview, page_index)) {
    return MuPdfDeferredStage::Preview;
  }

  if (!BitmapCacheValid(mupdf_state->current_interactive_tile, page_index) &&
      !HasMuPdfInteractiveRenderFailed(mupdf_state, page_index)) {
    return MuPdfDeferredStage::Interactive;
  }

  if (app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind) &&
      !HasMuPdfFinalRenderFailed(mupdf_state, page_index) &&
      (mupdf_state->final_cache_pending ||
       !BitmapCacheValid(mupdf_state->current_final_zoom, page_index))) {
    return MuPdfDeferredStage::Final;
  }

  if (HasMuPdfInteractiveRenderFailed(mupdf_state, page_index) ||
      HasMuPdfFinalRenderFailed(mupdf_state, page_index)) {
    return MuPdfDeferredStage::None;
  }

  if (!app_flow_utils::MuPdfShouldPrefetchAdjacent(
          mupdf_state->document_kind)) {
    return MuPdfDeferredStage::None;
  }

  const int next = page_index + 1;
  if (next < (int)mupdf_state->page_count) {
    const Book::MuPdfState::AdjacentSlot &slot = mupdf_state->next_slot;
    if (slot.page != next || !BitmapCacheValid(slot.preview, next) ||
        !BitmapCacheValid(slot.interactive_tile, next)) {
      return MuPdfDeferredStage::Prefetch;
    }
  }

  const int prev = page_index - 1;
  if (prev >= 0) {
    const Book::MuPdfState::AdjacentSlot &slot = mupdf_state->prev_slot;
    if (slot.page != prev || !BitmapCacheValid(slot.preview, prev) ||
        !BitmapCacheValid(slot.interactive_tile, prev)) {
      return MuPdfDeferredStage::Prefetch;
    }
  }

  return MuPdfDeferredStage::None;
}

static bool BlitBitmapCacheViewport(Text *ts, u16 *screen, int logical_height,
                                    int draw_width, int draw_height,
                                    const Book::MuPdfState::BitmapCache &cache,
                                    const pdf_view_utils::NormalizedRect &viewport,
                                    bool high_quality_filter) {
  if (!ts || !screen || !BitmapCacheValid(cache, cache.page))
    return false;

  const float cache_right = cache.left + cache.width;
  const float cache_bottom = cache.top + cache.height;
  if (viewport.left + viewport.width <= cache.left ||
      viewport.top + viewport.height <= cache.top || viewport.left >= cache_right ||
      viewport.top >= cache_bottom) {
    return false;
  }

  const float rel_left =
      std::max(0.0f, (viewport.left - cache.left) / std::max(0.0001f, cache.width));
  const float rel_top =
      std::max(0.0f, (viewport.top - cache.top) / std::max(0.0001f, cache.height));
  const float rel_right =
      std::min(1.0f, (viewport.left + viewport.width - cache.left) /
                         std::max(0.0001f, cache.width));
  const float rel_bottom =
      std::min(1.0f, (viewport.top + viewport.height - cache.top) /
                         std::max(0.0001f, cache.height));
  int crop_x = std::max(0, std::min(cache.bitmap_width - 1,
                                    (int)(rel_left * cache.bitmap_width)));
  int crop_y = std::max(0, std::min(cache.bitmap_height - 1,
                                    (int)(rel_top * cache.bitmap_height)));
  int crop_w = std::max(
      1, std::min(cache.bitmap_width - crop_x,
                  (int)((rel_right - rel_left) * cache.bitmap_width + 0.5f)));
  int crop_h = std::max(
      1, std::min(cache.bitmap_height - crop_y,
                  (int)((rel_bottom - rel_top) * cache.bitmap_height + 0.5f)));

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      ts, screen, logical_height, 0, 0, draw_width, draw_height, cache.PixelData(), cache.PixelCount(),
      cache.bitmap_width, cache.bitmap_height, crop_x, crop_y, crop_w, crop_h,
      high_quality_filter);
  return true;
}

static bool BlitBitmapCacheViewportRegion(
    Text *ts, u16 *screen, int logical_height, int draw_width, int draw_height,
    int dst_y0, int dst_y1, const Book::MuPdfState::BitmapCache &cache,
    const pdf_view_utils::NormalizedRect &viewport,
    bool high_quality_filter) {
  if (!ts || !screen || !BitmapCacheValid(cache, cache.page))
    return false;
  dst_y0 = std::max(0, dst_y0);
  dst_y1 = std::min(draw_height, dst_y1);
  if (dst_y0 >= dst_y1)
    return false;

  const float vp_h = std::max(0.0001f, viewport.height);
  const float region_top_norm = viewport.top + (float)dst_y0 / draw_height * vp_h;
  const float region_bot_norm = viewport.top + (float)dst_y1 / draw_height * vp_h;

  const float cache_h = std::max(0.0001f, cache.height);
  const float cache_w = std::max(0.0001f, cache.width);

  const float rel_left =
      std::max(0.0f, (viewport.left - cache.left) / cache_w);
  const float rel_right =
      std::min(1.0f, (viewport.left + viewport.width - cache.left) / cache_w);
  const float rel_top =
      std::max(0.0f, (region_top_norm - cache.top) / cache_h);
  const float rel_bottom =
      std::min(1.0f, (region_bot_norm - cache.top) / cache_h);

  if (rel_left >= rel_right || rel_top >= rel_bottom)
    return false;

  const int crop_x = std::max(0, std::min(cache.bitmap_width - 1,
                                          (int)(rel_left * cache.bitmap_width)));
  const int crop_y = std::max(0, std::min(cache.bitmap_height - 1,
                                          (int)(rel_top * cache.bitmap_height)));
  const int crop_w = std::max(
      1, std::min(cache.bitmap_width - crop_x,
                  (int)((rel_right - rel_left) * cache.bitmap_width + 0.5f)));
  const int crop_h = std::max(
      1, std::min(cache.bitmap_height - crop_y,
                  (int)((rel_bottom - rel_top) * cache.bitmap_height + 0.5f)));

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      ts, screen, logical_height, 0, dst_y0, draw_width, dst_y1 - dst_y0,
      cache.PixelData(), cache.PixelCount(), cache.bitmap_width, cache.bitmap_height, crop_x, crop_y,
      crop_w, crop_h, high_quality_filter);
  return true;
}

static bool BlitRawBitmapViewportRegion(
    Text *ts, u16 *screen, int logical_height, int draw_width, int draw_height,
    int dst_y0, int dst_y1, const std::vector<u16> &pixels, int bitmap_width,
    int bitmap_height, float cache_left, float cache_top, float cache_width,
    float cache_height, const pdf_view_utils::NormalizedRect &viewport) {
  if (!ts || !screen || pixels.empty() || bitmap_width <= 0 ||
      bitmap_height <= 0)
    return false;

  dst_y0 = std::max(0, dst_y0);
  dst_y1 = std::min(draw_height, dst_y1);
  if (dst_y0 >= dst_y1)
    return false;

  const float vp_h = std::max(0.0001f, viewport.height);
  const float region_top_norm =
      viewport.top + (float)dst_y0 / draw_height * vp_h;
  const float region_bot_norm =
      viewport.top + (float)dst_y1 / draw_height * vp_h;

  const float cache_h = std::max(0.0001f, cache_height);
  const float cache_w = std::max(0.0001f, cache_width);

  const float rel_left =
      std::max(0.0f, (viewport.left - cache_left) / cache_w);
  const float rel_right =
      std::min(1.0f, (viewport.left + viewport.width - cache_left) / cache_w);
  const float rel_top =
      std::max(0.0f, (region_top_norm - cache_top) / cache_h);
  const float rel_bottom =
      std::min(1.0f, (region_bot_norm - cache_top) / cache_h);

  if (rel_left >= rel_right || rel_top >= rel_bottom)
    return false;

  const int crop_x = std::max(0, std::min(bitmap_width - 1,
                                          (int)(rel_left * bitmap_width)));
  const int crop_y = std::max(0, std::min(bitmap_height - 1,
                                          (int)(rel_top * bitmap_height)));
  const int crop_w = std::max(
      1, std::min(bitmap_width - crop_x,
                  (int)((rel_right - rel_left) * bitmap_width + 0.5f)));
  const int crop_h = std::max(
      1, std::min(bitmap_height - crop_y,
                  (int)((rel_bottom - rel_top) * bitmap_height + 0.5f)));

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      ts, screen, logical_height, 0, dst_y0, draw_width, dst_y1 - dst_y0,
      pixels, bitmap_width, bitmap_height, crop_x, crop_y, crop_w, crop_h,
      false);
  return true;
}

bool Book::HasPendingMuPdfDeferredWork() const {
  if (!IsPdf() || !mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return false;
  if (debug_runtime::ForceSynchronousMuPdfRender())
    return false;

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentMuPdfViewport(mupdf_state);
  return GetNextMuPdfDeferredStage(mupdf_state, page_index, viewport) !=
         MuPdfDeferredStage::None;
}

void Book::CancelMuPdfIncrementalRender() {
  if (IsPdf() && mupdf_state)
    CancelMuPdfIncrementalRenderState(mupdf_state);
}

u32 Book::GetMuPdfDeferredDelayMs() const {
  if (!IsPdf() || !mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return 0;
  if (debug_runtime::ForceSynchronousMuPdfRender())
    return 0;

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentMuPdfViewport(mupdf_state);

  switch (GetNextMuPdfDeferredStage(mupdf_state, page_index, viewport)) {
  case MuPdfDeferredStage::Preview:
    return 0;
  case MuPdfDeferredStage::Interactive:
    return kPdfInteractiveDeferredDelayMs;
  case MuPdfDeferredStage::Final:
    // If a strip render is already in progress, pump the next strip quickly.
    // The long delay is only needed to detect idle before starting.
    return mupdf_state->incremental.active ? 50u : kPdfFinalDeferredDelayMs;
  case MuPdfDeferredStage::Prefetch:
    return kPdfPrefetchDeferredDelayMs;
  case MuPdfDeferredStage::None:
  default:
    return 0;
  }
}

void Book::DrawCurrentMuPdfView(Text *ts) {
  if (!ts || !IsPdf())
    return;
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc || mupdf_state->page_count == 0)
    return;

  const bool wide = ts->IsWideTopScreenActive();
  if (mupdf_state->wide_top_screen != wide) {
    CancelMuPdfIncrementalRenderState(mupdf_state);
    ResetBitmapCache(&mupdf_state->current_interactive_tile);
    ResetBitmapCache(&mupdf_state->current_final_zoom);
    ResetAdjacentSlot(&mupdf_state->prev_slot, mupdf_state->ctx);
    ResetAdjacentSlot(&mupdf_state->next_slot, mupdf_state->ctx);
    ResetMuPdfRenderFailureState(mupdf_state);
    mupdf_state->wide_top_screen = wide;
  }

  const fixed_layout_screen::TargetDimensions top_dims =
      fixed_layout_screen::TargetDims((unsigned char)GetOrientation(), true);
  const fixed_layout_screen::TargetDimensions bottom_dims =
      fixed_layout_screen::TargetDims((unsigned char)GetOrientation(), false);
  if (mupdf_state->target_top_width != top_dims.width ||
      mupdf_state->target_top_height != top_dims.height ||
      mupdf_state->target_bottom_width != bottom_dims.width ||
      mupdf_state->target_bottom_height != bottom_dims.height) {
    fixed_layout_viewport_utils::ResetViewportForTargetChange(
        &mupdf_state->viewport, pdf_view_utils::DefaultZoomIndex());
    CancelMuPdfIncrementalRenderState(mupdf_state);
    ResetBitmapCache(&mupdf_state->current_preview);
    ResetBitmapCache(&mupdf_state->current_interactive_tile);
    ResetBitmapCache(&mupdf_state->current_final_zoom);
    ResetAdjacentSlot(&mupdf_state->prev_slot, mupdf_state->ctx);
    ResetAdjacentSlot(&mupdf_state->next_slot, mupdf_state->ctx);
    ResetMuPdfRenderFailureState(mupdf_state);
    mupdf_state->target_top_width = top_dims.width;
    mupdf_state->target_top_height = top_dims.height;
    mupdf_state->target_bottom_width = bottom_dims.width;
    mupdf_state->target_bottom_height = bottom_dims.height;
  }

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  position = page_index;
  fixed_perf::BeginView(mupdf_state->doc, "PDF", GetFileName(), page_index,
                       mupdf_state->viewport.zoom_index,
                       mupdf_state->target_top_width, mupdf_state->target_top_height);
  const uint64_t perf_draw_start = fixed_perf::Now();
  uint64_t perf_phase = perf_draw_start;
  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: enter page=%d page_count=%u zoom=%d doc_kind=%d",
               page_index, (unsigned)mupdf_state->page_count,
               mupdf_state->viewport.zoom_index, (int)mupdf_state->document_kind);

  if (mupdf_state->current_preview.page != page_index)
    ResetBitmapCache(&mupdf_state->current_preview);
  if (mupdf_state->current_interactive_tile.page != page_index ||
      mupdf_state->current_interactive_tile.zoom_index != mupdf_state->viewport.zoom_index)
    ResetBitmapCache(&mupdf_state->current_interactive_tile);
  if (mupdf_state->current_final_zoom.page != page_index)
    ResetBitmapCache(&mupdf_state->current_final_zoom);

  EnsureMuPdfPageMetrics(mupdf_state, page_index);
  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: after-metrics page=%d size=(%.2f,%.2f)",
               page_index, (double)mupdf_state->page_width,
               (double)mupdf_state->page_height);
  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: preview-cache-begin page=%d", page_index);

  fixed_perf::ViewStage("pdf.prepare", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
  const bool preview_ok = EnsureCurrentMuPdfPreviewCache(mupdf_state, page_index);
  fixed_perf::ViewStage("pdf.ensure_preview", fixed_perf::Now() - perf_phase,
                        preview_ok ? 1 : 0);
  if (!preview_ok) {
    IStatusReporter *reporter = GetStatusReporter();
    if (reporter) {
      DBG_LOGF_CAT(reporter, DBG_LEVEL_WARN, DBG_CAT_RENDER,
                   "MuPDF preview failed page=%d page_count=%u doc_kind=%d",
                   page_index, (unsigned)mupdf_state->page_count,
                   (int)mupdf_state->document_kind);
    }
    const bool complex_page =
        (mupdf_state->page_too_complex_for_device == page_index);
    DrawMuPdfLoadFailure(this, ts, page_index, complex_page);
    return;
  }

  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: preview-cache-done page=%d bmp=%dx%d", page_index,
               mupdf_state->current_preview.bitmap_width,
               mupdf_state->current_preview.bitmap_height);

  perf_phase = fixed_perf::Now();
  // In synchronous mode, render the interactive tile immediately after preview.
  // This gives proper zoom-aware resolution without background threads.
  if (debug_runtime::ForceSynchronousMuPdfRender() &&
      (mupdf_state->wide_top_screen ||
       !BitmapCacheValid(mupdf_state->current_interactive_tile, page_index))) {
    EnsureCurrentMuPdfInteractiveTile(mupdf_state, page_index);
  }

  fixed_perf::ViewStage("pdf.ensure_interactive", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
  pdf_view_utils::NormalizedRect viewport = ComputeCurrentMuPdfViewport(mupdf_state);
  mupdf_state->viewport.center_x = viewport.left + viewport.width * 0.5f;
  mupdf_state->viewport.center_y = viewport.top + viewport.height * 0.5f;
  const bool has_final_cache =
      BitmapCacheValid(mupdf_state->current_final_zoom, page_index) &&
      mupdf_state->current_final_zoom.zoom_index >= mupdf_state->viewport.max_zoom_index;
  const bool has_interactive_tile =
      BitmapCacheValid(mupdf_state->current_interactive_tile, page_index);
  const bool wants_final_cache =
      app_flow_utils::MuPdfWantsFinalQualityRender(mupdf_state->document_kind);
  mupdf_state->final_cache_pending =
      wants_final_cache && !has_final_cache &&
      !HasMuPdfFinalRenderFailed(mupdf_state, page_index);
  const bool high_quality_viewport =
      !mupdf_state->viewport.interaction_active;
  const float preview_source_width =
      std::max(1.0f, (float)mupdf_state->current_preview.bitmap_width);
  const float preview_source_height =
      std::max(1.0f, (float)mupdf_state->current_preview.bitmap_height);
  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height, kPdfPreviewPadding,
          kPdfPreviewPadding,
          mupdf_state->target_bottom_width - 2 * kPdfPreviewPadding,
          mupdf_state->target_bottom_height - 2 * kPdfPreviewPadding);

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->margin.bottom = 0;

  fixed_perf::ViewStage("pdf.view_setup", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: left-screen-blit-begin page=%d final=%d interactive=%d incremental=%d",
               page_index, has_final_cache ? 1 : 0,
               has_interactive_tile ? 1 : 0,
               mupdf_state->incremental.active ? 1 : 0);

  if (has_final_cache) {
    BlitBitmapCacheViewport(ts, ts->screenleft, mupdf_state->target_top_height,
                            mupdf_state->target_top_width, mupdf_state->target_top_height,
                            mupdf_state->current_final_zoom, viewport,
                            high_quality_viewport);
  } else if (mupdf_state->incremental.active &&
             mupdf_state->incremental.strips_completed > 0 &&
             mupdf_state->incremental.target_page == page_index) {
    Book::MuPdfState::IncrementalRenderState &inc = mupdf_state->incremental;
    const int rendered_h = (inc.strips_completed * inc.partial_height) /
                           inc.strips_total;
    const float rendered_top_norm =
        (inc.partial_height > 0)
            ? (float)rendered_h / (float)inc.partial_height
            : 0.0f;

    const float vp_h = std::max(0.0001f, viewport.height);
    const float rendered_in_vp =
        std::min(rendered_top_norm, viewport.top + vp_h) - viewport.top;
    const int split_y = (rendered_in_vp > 0.0f)
        ? std::min(mupdf_state->target_top_height,
                   (int)(rendered_in_vp / vp_h * mupdf_state->target_top_height + 0.5f))
        : 0;

    if (split_y > 0) {
      BlitRawBitmapViewportRegion(ts, ts->screenleft, mupdf_state->target_top_height,
                                  mupdf_state->target_top_width, mupdf_state->target_top_height,
                                  0, split_y, inc.partial_pixels,
                                  inc.partial_width, inc.partial_height,
                                  0.0f, 0.0f, 1.0f, 1.0f, viewport);
    }
    if (split_y < mupdf_state->target_top_height) {
      // Cascade through available sources for the unrendered portion.
      // Each blit can fail if the cached bitmap doesn't cover the region
      // (e.g. stale interactive tile after viewport change on o3DS), so
      // check return values and fall through to the next source.
      bool bottom_ok = false;
      if (has_interactive_tile) {
        bottom_ok = BlitBitmapCacheViewportRegion(
            ts, ts->screenleft, mupdf_state->target_top_height, mupdf_state->target_top_width,
            mupdf_state->target_top_height, split_y, mupdf_state->target_top_height,
            mupdf_state->current_interactive_tile, viewport,
            high_quality_viewport);
      }
      if (!bottom_ok &&
          BitmapCacheValid(mupdf_state->current_preview, page_index)) {
        bottom_ok = BlitBitmapCacheViewportRegion(
            ts, ts->screenleft, mupdf_state->target_top_height, mupdf_state->target_top_width,
            mupdf_state->target_top_height, split_y, mupdf_state->target_top_height,
            mupdf_state->current_preview, viewport, high_quality_viewport);
      }
      if (!bottom_ok) {
        // Final fallback: blit the partial buffer itself — unrendered strips
        // are initialised to kPdfPaper (white), so the lower portion shows
        // paper colour instead of the background left by ClearScreen.
        BlitRawBitmapViewportRegion(ts, ts->screenleft, mupdf_state->target_top_height,
                                    mupdf_state->target_top_width, mupdf_state->target_top_height,
                                    split_y, mupdf_state->target_top_height,
                                    inc.partial_pixels,
                                    inc.partial_width, inc.partial_height,
                                    0.0f, 0.0f, 1.0f, 1.0f, viewport);
      }
    }
  } else {
    // No incremental render in progress — use best available full-page source.
    bool ok = false;
    if (has_interactive_tile) {
      ok = BlitBitmapCacheViewport(ts, ts->screenleft, mupdf_state->target_top_height,
                                   mupdf_state->target_top_width, mupdf_state->target_top_height,
                                   mupdf_state->current_interactive_tile,
                                   viewport, high_quality_viewport);
    }
    if (!ok && BitmapCacheValid(mupdf_state->current_preview, page_index)) {
      ok = BlitBitmapCacheViewport(ts, ts->screenleft, mupdf_state->target_top_height,
                                   mupdf_state->target_top_width, mupdf_state->target_top_height,
                                   mupdf_state->current_preview, viewport,
                                   high_quality_viewport);
    }
    if (!ok) {
      ts->SetPen(18, 28);
      ts->PrintString("MuPDF render unavailable");
    }
  }
  fixed_perf::ViewStage("pdf.blit_main", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  DrawBottomGradientBackground();
  ts->FillRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kPdfPaper);

  fixed_perf::ViewStage("pdf.preview_background", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
  if (!mupdf_state->current_preview.pixels.empty() &&
      mupdf_state->current_preview.bitmap_width > 0 &&
      mupdf_state->current_preview.bitmap_height > 0) {
    fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, mupdf_state->target_bottom_height, preview_layout.x,
        preview_layout.y, preview_layout.width, preview_layout.height,
        mupdf_state->current_preview.pixels,
        mupdf_state->current_preview.bitmap_width,
        mupdf_state->current_preview.bitmap_height, 0, 0,
        mupdf_state->current_preview.bitmap_width,
        mupdf_state->current_preview.bitmap_height, true);
  }
  fixed_perf::ViewStage("pdf.blit_preview", fixed_perf::Now() - perf_phase);
  perf_phase = fixed_perf::Now();
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
  fixed_perf::ViewStage("pdf.overlay", fixed_perf::Now() - perf_phase);
  fixed_perf::ViewStage("pdf.draw_total", fixed_perf::Now() - perf_draw_start);
  fixed_perf::Drawn(has_final_cache ? 3 : has_interactive_tile ? 2 : 1);
  DBG_LOGF_CAT(GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_RENDER,
               "MUPDF draw: end page=%d", page_index);
}

void Book::PrefetchAdjacentMuPdfPage() {
  if (!IsPdf() || !mupdf_state)
    return;
  if (!app_flow_utils::MuPdfShouldPrefetchAdjacent(mupdf_state->document_kind))
    return;
  PrepareAdjacentMuPdfSlot(
      mupdf_state, ClampMuPdfPageIndex(position, mupdf_state->page_count), 1);
}

bool Book::PumpDeferredMuPdfWork(u32 budget_ms) {
  (void)budget_ms;
  if (!IsPdf() || !mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return false;
  if (debug_runtime::ForceSynchronousMuPdfRender())
    return false;

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  const u64 start_ms = osGetTime();
  bool worked = false;
  if (!HasPendingMuPdfDeferredWork())
    return false;

  if (!BitmapCacheValid(mupdf_state->current_preview, page_index)) {
    if (EnsureCurrentMuPdfPreviewCache(mupdf_state, page_index))
      worked = true;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (!BitmapCacheValid(mupdf_state->current_interactive_tile, page_index) &&
      !HasMuPdfInteractiveRenderFailed(mupdf_state, page_index)) {
    if (EnsureCurrentMuPdfInteractiveTile(mupdf_state, page_index))
      worked = true;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (mupdf_state->final_cache_pending ||
      (!HasMuPdfFinalRenderFailed(mupdf_state, page_index) &&
       !BitmapCacheValid(mupdf_state->current_final_zoom, page_index)) ||
      mupdf_state->incremental.active) {
    if (PumpMuPdfIncrementalStrip(mupdf_state, page_index))
      worked = true;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (PrepareAdjacentMuPdfSlot(mupdf_state, page_index, 1))
    worked = true;
  if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
    return worked;

  if (PrepareAdjacentMuPdfSlot(mupdf_state, page_index, -1))
    worked = true;

  return worked;
}
