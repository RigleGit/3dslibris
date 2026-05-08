#include "book/book.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"
#include "formats/cbz/cbz_worker.h"
#include "formats/common/format_limits.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "formats/common/pdf_view_utils.h"
#include "settings/prefs.h"
#include "shared/debug_runtime_mode.h"
#include "ui/text.h"
#include "shared/debug_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

static const u32 kCbzInteractiveDeferredDelayMs = 180;
static const u32 kCbzPreloadDeferredDelayMs = 600;

inline bool CbzSourceValid(const Book::CbzState::PageBitmap &page_bitmap,
                           int page_index, int zoom_index) {
  return page_bitmap.page == page_index &&
         page_bitmap.zoom_index >= zoom_index &&
         page_bitmap.bitmap.width > 0 &&
         page_bitmap.bitmap.height > 0 &&
         !page_bitmap.bitmap.pixels.empty();
}

inline int ClampCbzPageIndex(int page_index, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (page_index < 0)
    return 0;
  if (page_index >= (int)page_count)
    return (int)page_count - 1;
  return page_index;
}

enum CbzWorkerQueueState {
  CBZ_WORKER_IDLE,
  CBZ_WORKER_PENDING,
  CBZ_WORKER_READY_TO_INTEGRATE
};

struct ScopedTextRenderState {
  explicit ScopedTextRenderState(Text *text)
      : ts(text),
        style(text ? text->GetStyle() : 0),
        color_mode(text ? text->GetColorMode() : 0),
        screen(text ? text->GetScreen() : NULL),
        bottom_margin(text ? text->margin.bottom : 0) {}

  ~ScopedTextRenderState() {
    if (!ts)
      return;
    ts->SetStyle(style);
    ts->SetColorMode(color_mode);
    ts->SetScreen(screen);
    ts->margin.bottom = bottom_margin;
  }

  Text *ts;
  int style;
  int color_mode;
  u16 *screen;
  int bottom_margin;

private:
  ScopedTextRenderState(const ScopedTextRenderState &);
  ScopedTextRenderState &operator=(const ScopedTextRenderState &);
};

CbzWorkerQueueState GetCbzWorkerQueueState(
    const Book::CbzState *cbz_state) {
  if (!cbz_state || !cbz_state->worker)
    return CBZ_WORKER_IDLE;

  const bool job_pending =
      __atomic_load_n(&cbz_state->worker->job_pending, __ATOMIC_ACQUIRE);

  if (job_pending)
    return CBZ_WORKER_PENDING;

  if (cbz_state->worker->job_submitted)
    return CBZ_WORKER_READY_TO_INTEGRATE;

  return CBZ_WORKER_IDLE;
}

void ResetCbzPageBitmap(Book::CbzState::PageBitmap *page_bitmap) {
  if (!page_bitmap)
    return;
  page_bitmap->page = -1;
  page_bitmap->zoom_index = -1;
  page_bitmap->original_width = 0;
  page_bitmap->original_height = 0;
  page_bitmap->bitmap.width = 0;
  page_bitmap->bitmap.height = 0;
  page_bitmap->bitmap.pixels.clear();
}

bool DecodeCbzPageImageWithFallback(const std::vector<unsigned char> &bytes,
                                    int preferred_zoom_index,
                                    CbzDecodedPage *decoded,
                                    int *used_zoom_index) {
  if (!decoded)
    return false;
  for (int try_zoom = preferred_zoom_index; try_zoom >= 0; --try_zoom) {
    if (DecodeCbzPageImage(bytes, try_zoom, decoded)) {
      if (used_zoom_index)
        *used_zoom_index = try_zoom;
      return true;
    }
  }
  if (used_zoom_index)
    *used_zoom_index = -1;
  return false;
}

void DrawCbzLoadFailure(Book *book, Text *ts, int page_index,
                        const std::string &reason) {
  if (!book || !ts)
    return;

  ScopedTextRenderState saved_state(ts);
  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  ts->SetPen(14, 28);
  ts->PrintString("CBZ page unavailable");
  ts->SetPen(14, 52);
  char page_msg[48];
  snprintf(page_msg, sizeof(page_msg), "page %d", page_index + 1);
  ts->PrintString(page_msg);

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  book->DrawBottomGradientBackground();
  ts->SetPen(12, 28);
  ts->PrintString("image decode failed");
  ts->SetPen(12, 48);
  ts->PrintString("use L/R or B to leave");
  if (!reason.empty()) {
    ts->SetPen(12, 72);
    ts->PrintString(reason.c_str());
  }
}

pdf_view_utils::NormalizedRect ComputeCurrentCbzViewport(
    const Book::CbzState *cbz_state) {
  return pdf_view_utils::ComputeViewportRect(
      cbz_state ? cbz_state->page_width : 1.0f,
      cbz_state ? cbz_state->page_height : 1.0f,
      cbz_state ? pdf_view_utils::ZoomForIndex(cbz_state->viewport.zoom_index) : 1.0f,
      (float)fixed_layout_screen::kTopScreenWidth,
      (float)fixed_layout_screen::kTopScreenHeight,
      cbz_state ? cbz_state->viewport.center_x : 0.5f,
      cbz_state ? cbz_state->viewport.center_y : 0.5f);
}

bool PromoteCbzAdjacentSlotIfMatching(Book::CbzState *cbz_state,
                                      int page_index) {
  if (!cbz_state)
    return false;

  Book::CbzState::AdjacentSlot *slot = NULL;
  if (cbz_state->prev_slot.page == page_index)
    slot = &cbz_state->prev_slot;
  else if (cbz_state->next_slot.page == page_index)
    slot = &cbz_state->next_slot;
  if (!slot)
    return false;

  ResetCbzPageBitmap(&cbz_state->current_source);
  ResetCbzBitmapCache(&cbz_state->current_preview);
  ResetCbzBitmapCache(&cbz_state->current_interactive);
  cbz_state->page_width = std::max(1.0f, slot->page_width);
  cbz_state->page_height = std::max(1.0f, slot->page_height);
  cbz_state->current_preview = slot->preview;
  cbz_state->current_interactive = slot->interactive;
  ResetCbzAdjacentSlot(slot);
  cbz_state->preload_pending = false;
  return true;
}

bool EnsureCbzSourceLoaded(Book::CbzState *cbz_state, int page_index,
                           int zoom_index) {
  if (!cbz_state || page_index < 0 || page_index >= (int)cbz_state->entries.size())
    return false;
  if (cbz_state->failed_page == page_index)
    return false;
  if (CbzSourceValid(cbz_state->current_source, page_index, zoom_index))
    return true;

  std::vector<unsigned char> bytes;
  if (!ReadCbzArchiveEntryBytes(cbz_state->archive_path,
                                cbz_state->entries[(size_t)page_index], &bytes,
                                format_limits::kMaxCbzPageEntryBytes)) {
    cbz_state->last_error = GetLastCbzArchiveError();
    cbz_state->failed_page = page_index;
    return false;
  }

  CbzDecodedPage decoded;
  int used_zoom_index = -1;
  if (!DecodeCbzPageImageWithFallback(bytes, zoom_index, &decoded,
                                      &used_zoom_index)) {
    cbz_state->last_error = GetLastCbzDecodeError();
    cbz_state->failed_page = page_index;
    return false;
  }

  cbz_state->failed_page = -1;
  cbz_state->logged_failed_page = -1;
  cbz_state->last_error.clear();
  ResetCbzPageBitmap(&cbz_state->current_source);
  cbz_state->current_source.page = page_index;
  cbz_state->current_source.zoom_index = used_zoom_index;
  cbz_state->current_source.original_width = decoded.original_width;
  cbz_state->current_source.original_height = decoded.original_height;
  cbz_state->current_source.bitmap.width = decoded.source_bitmap.width;
  cbz_state->current_source.bitmap.height = decoded.source_bitmap.height;
  cbz_state->current_source.bitmap.pixels.swap(decoded.source_bitmap.pixels);
  cbz_state->page_width = (float)std::max(1, decoded.original_width);
  cbz_state->page_height = (float)std::max(1, decoded.original_height);
  return true;
}

bool EnsureCbzPreviewCache(Book::CbzState *cbz_state, int page_index) {
  if (!cbz_state)
    return false;
  PromoteCbzAdjacentSlotIfMatching(cbz_state, page_index);
  if (CbzPreviewCacheValid(cbz_state->current_preview, page_index))
    return true;
  if (!EnsureCbzSourceLoaded(cbz_state, page_index, 0))
    return false;

  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayout(
          cbz_state->page_width, cbz_state->page_height,
          fixed_layout_screen::kBottomScreenWidth -
              2 * fixed_layout_preview::kPadding,
          fixed_layout_screen::kBottomScreenHeight -
              2 * fixed_layout_preview::kPadding);
  CbzBitmap scaled;
  if (!ScaleCbzBitmap(cbz_state->current_source.bitmap,
                      std::max(1, preview_layout.width),
                      std::max(1, preview_layout.height), true, &scaled)) {
    return false;
  }

  cbz_state->current_preview.page = page_index;
  cbz_state->current_preview.zoom_index = -1;
  cbz_state->current_preview.bitmap_width = scaled.width;
  cbz_state->current_preview.bitmap_height = scaled.height;
  cbz_state->current_preview.pixels.swap(scaled.pixels);
  return true;
}

bool EnsureCbzInteractiveCache(Book::CbzState *cbz_state, int page_index) {
  if (!cbz_state)
    return false;
  PromoteCbzAdjacentSlotIfMatching(cbz_state, page_index);
  if (CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                          cbz_state->viewport.zoom_index)) {
    return true;
  }
  if (!EnsureCbzSourceLoaded(cbz_state, page_index, cbz_state->viewport.zoom_index))
    return false;

  const float fit_scale =
      std::min((float)fixed_layout_screen::kTopScreenWidth /
                   std::max(1.0f, cbz_state->page_width),
               (float)fixed_layout_screen::kTopScreenHeight /
                   std::max(1.0f, cbz_state->page_height));
  const float zoom = pdf_view_utils::ZoomForIndex(cbz_state->viewport.zoom_index);
  const int target_width = std::max(
      1, std::min(cbz_state->current_source.bitmap.width,
                  (int)(cbz_state->page_width * fit_scale * zoom + 0.5f)));
  const int target_height = std::max(
      1, std::min(cbz_state->current_source.bitmap.height,
                  (int)(cbz_state->page_height * fit_scale * zoom + 0.5f)));

  CbzBitmap scaled;
  // The interactive cache is for drag/page-turn responsiveness, not final
  // quality. Use the fast scaler here and keep HQ filtering only at blit time
  // when the viewer is idle.
  if (!ScaleCbzBitmap(cbz_state->current_source.bitmap, target_width,
                      target_height, false, &scaled)) {
    return false;
  }

  cbz_state->current_interactive.page = page_index;
  cbz_state->current_interactive.zoom_index = cbz_state->viewport.zoom_index;
  cbz_state->current_interactive.bitmap_width = scaled.width;
  cbz_state->current_interactive.bitmap_height = scaled.height;
  cbz_state->current_interactive.pixels.swap(scaled.pixels);
  return true;
}

bool BlitCbzCacheViewport(Text *ts, u16 *screen, int logical_height,
                          int draw_width, int draw_height,
                          const Book::CbzState::BitmapCache &cache,
                          const pdf_view_utils::NormalizedRect &viewport,
                          bool high_quality_filter) {
  if (!ts || !screen || cache.bitmap_width <= 0 || cache.bitmap_height <= 0 ||
      cache.pixels.empty())
    return false;

  const int crop_x = std::max(
      0, std::min(cache.bitmap_width - 1,
                  (int)(viewport.left * cache.bitmap_width + 0.5f)));
  const int crop_y = std::max(
      0, std::min(cache.bitmap_height - 1,
                  (int)(viewport.top * cache.bitmap_height + 0.5f)));
  const int crop_w = std::max(
      1, std::min(cache.bitmap_width - crop_x,
                  (int)(viewport.width * cache.bitmap_width + 0.5f)));
  const int crop_h = std::max(
      1, std::min(cache.bitmap_height - crop_y,
                  (int)(viewport.height * cache.bitmap_height + 0.5f)));

  const pdf_view_utils::PreviewLayout layout =
      pdf_view_utils::ComputePreviewLayout((float)crop_w, (float)crop_h,
                                           draw_width, draw_height);

  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      ts, screen, logical_height, layout.x, layout.y, layout.width,
      layout.height, cache.pixels,
      cache.bitmap_width, cache.bitmap_height, crop_x, crop_y, crop_w, crop_h,
      high_quality_filter);
  return true;
}

void DrawCbzPreviewPanel(Book *book, Text *ts,
                         const Book::CbzState *cbz_state,
                         int page_index,
                         const pdf_view_utils::PreviewLayout &preview_layout,
                         const pdf_view_utils::NormalizedRect &viewport) {
  if (!book || !ts || !cbz_state)
    return;

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  book->DrawBottomGradientBackground();
  ts->FillRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height),
               fixed_layout_preview::kPaper);
  if (CbzPreviewCacheValid(cbz_state->current_preview, page_index)) {
    fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, fixed_layout_screen::kBottomScreenHeight,
        preview_layout.x, preview_layout.y, preview_layout.width,
        preview_layout.height, cbz_state->current_preview.pixels,
        cbz_state->current_preview.bitmap_width,
        cbz_state->current_preview.bitmap_height, 0, 0,
        cbz_state->current_preview.bitmap_width,
        cbz_state->current_preview.bitmap_height, true);
  }
  ts->DrawRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height),
               fixed_layout_preview::kFrame);

  const int viewport_x =
      preview_layout.x + (int)(viewport.left * preview_layout.width + 0.5f);
  const int viewport_y =
      preview_layout.y + (int)(viewport.top * preview_layout.height + 0.5f);
  const int viewport_w =
      std::max(1, (int)(viewport.width * preview_layout.width + 0.5f));
  const int viewport_h =
      std::max(1, (int)(viewport.height * preview_layout.height + 0.5f));
  ts->DrawRect((u16)viewport_x, (u16)viewport_y,
               (u16)(viewport_x + viewport_w),
               (u16)(viewport_y + viewport_h),
               fixed_layout_preview::kViewportAccent);
}

bool HasCbzNeighborPending(const Book::CbzState *cbz_state, int page_index) {
  if (!cbz_state || cbz_state->page_count == 0)
    return false;
  const int next = page_index + 1;
  if (next < (int)cbz_state->page_count && cbz_state->failed_page != next &&
      !(cbz_state->next_slot.page == next &&
        CbzPreviewCacheValid(cbz_state->next_slot.preview, next) &&
        CbzBitmapCacheValid(cbz_state->next_slot.interactive, next,
                            cbz_state->viewport.zoom_index))) {
    return true;
  }
  const int prev = page_index - 1;
  if (prev >= 0 && cbz_state->failed_page != prev &&
      !(cbz_state->prev_slot.page == prev &&
        CbzPreviewCacheValid(cbz_state->prev_slot.preview, prev) &&
        CbzBitmapCacheValid(cbz_state->prev_slot.interactive, prev,
                            cbz_state->viewport.zoom_index))) {
    return true;
  }
  return false;
}

} // namespace

void Book::ResetCbzTransientViewState(bool restart_worker) {
  if (!IsCbz() || !cbz_state)
    return;

  cbz_state->viewport.interaction_active = false;
  cbz_state->preload_pending = false;
  cbz_state->viewport.center_x = 0.5f;
  cbz_state->viewport.center_y = 0.5f;
  ResetCbzFailureState();
  ResetCbzPageBitmap(&cbz_state->current_source);
  ResetCbzBitmapCache(&cbz_state->current_preview);
  ResetCbzBitmapCache(&cbz_state->current_interactive);
  ResetCbzAdjacentSlot(&cbz_state->prev_slot);
  ResetCbzAdjacentSlot(&cbz_state->next_slot);

  if (!restart_worker)
    return;

  ShutdownCbzWorker(cbz_state);
  InitCbzWorker(cbz_state);
}

void Book::DrawCurrentCbzView(Text *ts) {
  if (!ts || !IsCbz() || !cbz_state)
    return;

  if (cbz_state->page_count == 0)
    return;

  const int page_index = ClampCbzPageIndex(GetPosition(), cbz_state->page_count);
  SetPosition(page_index);

  if (cbz_state->current_preview.page != page_index)
    ResetCbzBitmapCache(&cbz_state->current_preview);
  if (cbz_state->current_interactive.page != page_index)
    ResetCbzBitmapCache(&cbz_state->current_interactive);

  if (!EnsureCbzPreviewCache(cbz_state, page_index)) {
    IStatusReporter *reporter = GetStatusReporter();
    if (reporter && cbz_state->failed_page == page_index &&
        cbz_state->logged_failed_page != page_index) {
      DBG_LOGF_CAT(reporter, DBG_LEVEL_WARN, DBG_CAT_RENDER,
                   "CBZ page load failed page=%d path=%s entry=%s reason=%s",
                   page_index, cbz_state->archive_path.c_str(),
                   cbz_state->entries[(size_t)page_index].normalized_path.c_str(),
                   cbz_state->last_error.c_str());
      cbz_state->logged_failed_page = page_index;
    }
    if (reporter) {
      DBG_LOGF(reporter, "CBZ load failed: showing fallback page=%d reason=%s",
               page_index, cbz_state->last_error.c_str());
    }
    DrawCbzLoadFailure(this, ts, page_index, cbz_state->last_error);
    return;
  }

  // In synchronous mode, render interactive cache immediately after preview.
  // This gives proper zoom-aware resolution without background threads.
  if (debug_runtime::ForceSynchronousCbzDecode() &&
      !CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->viewport.zoom_index)) {
    EnsureCbzInteractiveCache(cbz_state, page_index);
  }

  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentCbzViewport(cbz_state);
  cbz_state->viewport.center_x = viewport.left + viewport.width * 0.5f;
  cbz_state->viewport.center_y = viewport.top + viewport.height * 0.5f;
  const bool has_interactive = CbzBitmapCacheValid(
      cbz_state->current_interactive, page_index, cbz_state->viewport.zoom_index);
  const bool high_quality_viewport = !cbz_state->viewport.interaction_active;
  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          cbz_state->page_width, cbz_state->page_height,
          fixed_layout_preview::kPadding, fixed_layout_preview::kPadding,
          fixed_layout_screen::kBottomScreenWidth -
              2 * fixed_layout_preview::kPadding,
          fixed_layout_screen::kBottomScreenHeight -
              2 * fixed_layout_preview::kPadding);

  ScopedTextRenderState saved_state(ts);
  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  if (has_interactive) {
    BlitCbzCacheViewport(ts, ts->screenleft,
                         fixed_layout_screen::kTopScreenHeight,
                         fixed_layout_screen::kTopScreenWidth,
                         fixed_layout_screen::kTopScreenHeight,
                         cbz_state->current_interactive, viewport,
                         high_quality_viewport);
  } else {
    BlitCbzCacheViewport(ts, ts->screenleft,
                         fixed_layout_screen::kTopScreenHeight,
                         fixed_layout_screen::kTopScreenWidth,
                         fixed_layout_screen::kTopScreenHeight,
                         cbz_state->current_preview, viewport,
                         high_quality_viewport);
  }
  DrawCbzPreviewPanel(this, ts, cbz_state, page_index, preview_layout,
                      viewport);
}

void Book::SetCbzViewportInteraction(bool active) {
  if (!IsCbz() || !cbz_state)
    return;
  cbz_state->viewport.interaction_active = active;
}

void Book::ResetCbzViewport() {
  if (!IsCbz() || !cbz_state)
    return;
  const fixed_layout_viewport_utils::PageTurnDirection direction =
      (GetPrefs() && GetPrefs()->fixed_layout_rtl)
          ? fixed_layout_viewport_utils::PAGE_TURN_RIGHT_TO_LEFT
          : fixed_layout_viewport_utils::PAGE_TURN_LEFT_TO_RIGHT;
  const fixed_layout_viewport_utils::ViewportCenter center =
      fixed_layout_viewport_utils::DefaultPageTurnViewportCenter(direction);
  cbz_state->viewport.center_x = center.x;
  cbz_state->viewport.center_y = center.y;
  cbz_state->viewport.interaction_active = false;
}

bool Book::ChangeCbzZoom(int delta) {
  if (!IsCbz() || !cbz_state || delta == 0)
    return false;

  const int next = std::min(
      cbz_state->viewport.max_zoom_index,
      pdf_view_utils::ClampZoomIndexForDevice(cbz_state->viewport.zoom_index + delta,
                                              cbz_state->is_new_3ds));
  if (next == cbz_state->viewport.zoom_index)
    return false;

  cbz_state->viewport.zoom_index = next;
  ResetCbzBitmapCache(&cbz_state->current_interactive);
  ResetCbzAdjacentSlot(&cbz_state->prev_slot);
  ResetCbzAdjacentSlot(&cbz_state->next_slot);
  cbz_state->preload_pending = false;
  return true;
}

bool Book::MoveCbzViewportToPreview(int touch_x, int touch_y) {
  if (!IsCbz() || !cbz_state)
    return false;

  const pdf_view_utils::PreviewLayout preview =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          cbz_state->page_width, cbz_state->page_height,
          fixed_layout_preview::kPadding, fixed_layout_preview::kPadding,
          fixed_layout_screen::kBottomScreenWidth -
              2 * fixed_layout_preview::kPadding,
          fixed_layout_screen::kBottomScreenHeight -
              2 * fixed_layout_preview::kPadding);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentCbzViewport(cbz_state);
  const pdf_view_utils::NormalizedPoint center =
      pdf_view_utils::RecenterViewportFromPreview(preview, viewport, touch_x,
                                                  touch_y);
  const float dx = std::abs(center.x - cbz_state->viewport.center_x);
  const float dy = std::abs(center.y - cbz_state->viewport.center_y);
  if (dx < 0.0005f && dy < 0.0005f)
    return false;

  cbz_state->viewport.center_x = center.x;
  cbz_state->viewport.center_y = center.y;
  return true;
}

bool Book::TranslateCbzViewport(float dx, float dy) {
  if (!IsCbz() || !cbz_state)
    return false;
  const float new_x =
      std::max(0.0f, std::min(1.0f, cbz_state->viewport.center_x + dx));
  const float new_y =
      std::max(0.0f, std::min(1.0f, cbz_state->viewport.center_y + dy));
  if (std::abs(new_x - cbz_state->viewport.center_x) < 0.0005f &&
      std::abs(new_y - cbz_state->viewport.center_y) < 0.0005f)
    return false;
  cbz_state->viewport.center_x = new_x;
  cbz_state->viewport.center_y = new_y;
  return true;
}

bool Book::JumpCbzChapter(int delta) {
  (void)delta;
  return false;
}

bool Book::HasPendingCbzDeferredWork() const {
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return false;
  if (debug_runtime::ForceSynchronousCbzDecode())
    return false;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  if (cbz_state->failed_page == page_index)
    return false;
  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index))
    return true;
  if (!CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->viewport.zoom_index)) {
    return true;
  }
  switch (GetCbzWorkerQueueState(cbz_state)) {
    case CBZ_WORKER_READY_TO_INTEGRATE:
      return true;
    case CBZ_WORKER_PENDING:
      return false;
    case CBZ_WORKER_IDLE:
      break;
  }
  return HasCbzNeighborPending(cbz_state, page_index);
}

u32 Book::GetCbzDeferredDelayMs() const {
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return 0;
  if (debug_runtime::ForceSynchronousCbzDecode())
    return 0;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index) ||
      !CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->viewport.zoom_index)) {
    return kCbzInteractiveDeferredDelayMs;
  }
  switch (GetCbzWorkerQueueState(cbz_state)) {
    case CBZ_WORKER_READY_TO_INTEGRATE:
    case CBZ_WORKER_PENDING:
      return 0;
    case CBZ_WORKER_IDLE:
      break;
  }
  return HasCbzNeighborPending(cbz_state, page_index)
             ? kCbzPreloadDeferredDelayMs
             : 0;
}

bool Book::PumpDeferredCbzWork(u32 budget_ms) {
  (void)budget_ms;
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return false;
  if (debug_runtime::ForceSynchronousCbzDecode())
    return false;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  const u64 start_ms = osGetTime();
  bool worked = false;

  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index)) {
    const bool preview_worked = EnsureCbzPreviewCache(cbz_state, page_index);
    worked = preview_worked || worked;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (!CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->viewport.zoom_index)) {
    if (EnsureCbzInteractiveCache(cbz_state, page_index))
      worked = true;
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  const CbzPreloadPumpResult preload_result =
      PumpCbzPreloadWorker(cbz_state, page_index);
  return worked || preload_result == CbzPreloadPumpResult::Integrated;
}

void Book::CancelCbzDeferredWork() {
  if (!IsCbz() || !cbz_state)
    return;
  cbz_state->preload_pending = false;
}
