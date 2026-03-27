#include "formats/cbz/cbz_view.h"

#include "app/app.h"
#include "book/book.h"
#include "color_utils.h"
#include "debug_log.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"
#include "formats/cbz/cbz_worker.h"
#include "shared/pdf_view_utils.h"
#include "ui/text.h"

#include <algorithm>
#include <cmath>

extern App *app;

namespace {

static const int kCbzPreviewScreenWidth = 240;
static const int kCbzPreviewScreenHeight = 320;
static const int kCbzPreviewPadding = 4;
static const int kCbzZoomScreenWidth = 240;
static const int kCbzZoomScreenHeight = 400;
static const u16 kCbzPaper = 0xFFFF;
static const u16 kCbzFrame = 0x2104;
static const u16 kCbzAccent = 0x0000;
static const u32 kCbzInteractiveDeferredDelayMs = 180;
static const u32 kCbzPreloadDeferredDelayMs = 600;
static const size_t kCbzMaxEntryBytes = 64u * 1024u * 1024u;

inline bool CbzSourceValid(const Book::CbzState::PageBitmap &page_bitmap,
                           int page_index) {
  return page_bitmap.page == page_index &&
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

inline void UnpackRgb565(u16 pixel, int *r, int *g, int *b) {
  const int r5 = (pixel >> 11) & 0x1F;
  const int g6 = (pixel >> 5) & 0x3F;
  const int b5 = pixel & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

void ResetCbzPageBitmap(Book::CbzState::PageBitmap *page_bitmap) {
  if (!page_bitmap)
    return;
  page_bitmap->page = -1;
  page_bitmap->original_width = 0;
  page_bitmap->original_height = 0;
  page_bitmap->bitmap.width = 0;
  page_bitmap->bitmap.height = 0;
  page_bitmap->bitmap.pixels.clear();
}

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width, int draw_height,
                                const std::vector<u16> &pixels, int src_width,
                                int src_height, int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter) {
  if (!ts || !screen || pixels.empty() || draw_width <= 0 || draw_height <= 0 ||
      src_width <= 0 || src_height <= 0 || crop_width <= 0 ||
      crop_height <= 0) {
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
    for (int col = 0; col < draw_width; col++) {
      const int dx = x + col;
      if (dx < 0 || dx >= logical_width)
        continue;

      if (!high_quality_filter) {
        const int src_x = crop_x +
                          ((col * crop_width) / std::max(1, draw_width));
        const int src_y = crop_y +
                          ((row * crop_height) / std::max(1, draw_height));
        screen[(size_t)dy * (size_t)stride + (size_t)dx] =
            pixels[(size_t)src_y * (size_t)src_width + (size_t)src_x];
        continue;
      }

      const float src_xf =
          (float)crop_x +
          (((float)col + 0.5f) * (float)crop_width / (float)draw_width) - 0.5f;
      const float src_yf =
          (float)crop_y +
          (((float)row + 0.5f) * (float)crop_height / (float)draw_height) - 0.5f;
      const float clamped_x =
          std::max((float)crop_x,
                   std::min((float)(crop_x + crop_width - 1), src_xf));
      const float clamped_y =
          std::max((float)crop_y,
                   std::min((float)(crop_y + crop_height - 1), src_yf));
      const int x0 = (int)clamped_x;
      const int y0 = (int)clamped_y;
      const int x1 = std::min(crop_x + crop_width - 1, x0 + 1);
      const int y1 = std::min(crop_y + crop_height - 1, y0 + 1);
      const float tx = clamped_x - (float)x0;
      const float ty = clamped_y - (float)y0;

      int r00 = 0, g00 = 0, b00 = 0;
      int r10 = 0, g10 = 0, b10 = 0;
      int r01 = 0, g01 = 0, b01 = 0;
      int r11 = 0, g11 = 0, b11 = 0;
      UnpackRgb565(pixels[(size_t)y0 * (size_t)src_width + (size_t)x0], &r00,
                   &g00, &b00);
      UnpackRgb565(pixels[(size_t)y0 * (size_t)src_width + (size_t)x1], &r10,
                   &g10, &b10);
      UnpackRgb565(pixels[(size_t)y1 * (size_t)src_width + (size_t)x0], &r01,
                   &g01, &b01);
      UnpackRgb565(pixels[(size_t)y1 * (size_t)src_width + (size_t)x1], &r11,
                   &g11, &b11);

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;
      screen[(size_t)dy * (size_t)stride + (size_t)dx] = RGB565FromU8(
          r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 0.5f,
          g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 0.5f,
          b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 0.5f);
    }
  }
}

pdf_view_utils::NormalizedRect ComputeCurrentCbzViewport(
    const Book::CbzState *cbz_state) {
  return pdf_view_utils::ComputeViewportRect(
      cbz_state ? cbz_state->page_width : 1.0f,
      cbz_state ? cbz_state->page_height : 1.0f,
      cbz_state ? pdf_view_utils::ZoomForIndex(cbz_state->zoom_index) : 1.0f,
      (float)kCbzZoomScreenWidth, (float)kCbzZoomScreenHeight,
      cbz_state ? cbz_state->viewport_center_x : 0.5f,
      cbz_state ? cbz_state->viewport_center_y : 0.5f);
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

bool EnsureCbzSourceLoaded(Book::CbzState *cbz_state, int page_index) {
  if (!cbz_state || page_index < 0 || page_index >= (int)cbz_state->entries.size())
    return false;
  if (cbz_state->failed_page == page_index)
    return false;
  if (CbzSourceValid(cbz_state->current_source, page_index))
    return true;

  std::vector<unsigned char> bytes;
  if (!ReadCbzArchiveEntryBytes(cbz_state->archive_path,
                                cbz_state->entries[(size_t)page_index], &bytes,
                                kCbzMaxEntryBytes)) {
    cbz_state->failed_page = page_index;
    DBG_LOGF(app, "CBZ: source read failed page=%d path=%s", page_index,
             cbz_state->entries[(size_t)page_index].normalized_path.c_str());
    return false;
  }

  CbzDecodedPage decoded;
  if (!DecodeCbzPageImage(bytes, cbz_state->max_zoom_index, &decoded)) {
    cbz_state->failed_page = page_index;
    DBG_LOGF(app, "CBZ: source decode failed page=%d bytes=%zu err=%s path=%s",
             page_index, bytes.size(),
             GetLastCbzDecodeError(),
             cbz_state->entries[(size_t)page_index].normalized_path.c_str());
    return false;
  }

  cbz_state->failed_page = -1;
  ResetCbzPageBitmap(&cbz_state->current_source);
  cbz_state->current_source.page = page_index;
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
  if (!EnsureCbzSourceLoaded(cbz_state, page_index))
    return false;

  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayout(
          cbz_state->page_width, cbz_state->page_height,
          kCbzPreviewScreenWidth - 2 * kCbzPreviewPadding,
          kCbzPreviewScreenHeight - 2 * kCbzPreviewPadding);
  CbzBitmap scaled;
  if (!ScaleCbzBitmap(cbz_state->current_source.bitmap,
                      std::max(1, preview_layout.width),
                      std::max(1, preview_layout.height), true, &scaled)) {
    DBG_LOGF(app, "CBZ: preview scale failed page=%d src=%dx%d dst=%dx%d",
             page_index, cbz_state->current_source.bitmap.width,
             cbz_state->current_source.bitmap.height,
             std::max(1, preview_layout.width),
             std::max(1, preview_layout.height));
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
                          cbz_state->zoom_index)) {
    return true;
  }
  if (!EnsureCbzSourceLoaded(cbz_state, page_index))
    return false;

  const float fit_scale =
      std::min((float)kCbzZoomScreenWidth /
                   std::max(1.0f, cbz_state->page_width),
               (float)kCbzZoomScreenHeight /
                   std::max(1.0f, cbz_state->page_height));
  const float zoom = pdf_view_utils::ZoomForIndex(cbz_state->zoom_index);
  const int target_width = std::max(
      1, std::min(cbz_state->current_source.bitmap.width,
                  (int)(cbz_state->page_width * fit_scale * zoom + 0.5f)));
  const int target_height = std::max(
      1, std::min(cbz_state->current_source.bitmap.height,
                  (int)(cbz_state->page_height * fit_scale * zoom + 0.5f)));

  CbzBitmap scaled;
  if (!ScaleCbzBitmap(cbz_state->current_source.bitmap, target_width,
                      target_height, true, &scaled)) {
    return false;
  }

  cbz_state->current_interactive.page = page_index;
  cbz_state->current_interactive.zoom_index = cbz_state->zoom_index;
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

  BlitRgb565BitmapScaledCrop(
      ts, screen, logical_height, layout.x, layout.y, layout.width,
      layout.height, cache.pixels,
      cache.bitmap_width, cache.bitmap_height, crop_x, crop_y, crop_w, crop_h,
      high_quality_filter);
  return true;
}

bool HasCbzNeighborPending(const Book::CbzState *cbz_state, int page_index) {
  if (!cbz_state || cbz_state->page_count == 0)
    return false;
  const int next = page_index + 1;
  if (next < (int)cbz_state->page_count && cbz_state->failed_page != next &&
      !(cbz_state->next_slot.page == next &&
        CbzPreviewCacheValid(cbz_state->next_slot.preview, next) &&
        CbzBitmapCacheValid(cbz_state->next_slot.interactive, next,
                            cbz_state->zoom_index))) {
    return true;
  }
  const int prev = page_index - 1;
  if (prev >= 0 && cbz_state->failed_page != prev &&
      !(cbz_state->prev_slot.page == prev &&
        CbzPreviewCacheValid(cbz_state->prev_slot.preview, prev) &&
        CbzBitmapCacheValid(cbz_state->prev_slot.interactive, prev,
                            cbz_state->zoom_index))) {
    return true;
  }
  return false;
}

} // namespace

void DrawCurrentCbzView(Book *book, Text *ts) {
  if (!book || !ts || !book->IsCbz() || !book->cbz_state)
    return;

  Book::CbzState *cbz_state = book->cbz_state;
  if (cbz_state->page_count == 0)
    return;

  const int page_index = ClampCbzPageIndex(book->GetPosition(), cbz_state->page_count);
  book->SetPosition(page_index);

  if (cbz_state->current_preview.page != page_index)
    ResetCbzBitmapCache(&cbz_state->current_preview);
  if (cbz_state->current_interactive.page != page_index)
    ResetCbzBitmapCache(&cbz_state->current_interactive);

  if (!EnsureCbzPreviewCache(cbz_state, page_index))
    return;

  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentCbzViewport(cbz_state);
  cbz_state->viewport_center_x = viewport.left + viewport.width * 0.5f;
  cbz_state->viewport_center_y = viewport.top + viewport.height * 0.5f;
  const bool has_interactive = CbzBitmapCacheValid(
      cbz_state->current_interactive, page_index, cbz_state->zoom_index);
  const bool high_quality_viewport = !cbz_state->viewport_interaction_active;
  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          cbz_state->page_width, cbz_state->page_height, kCbzPreviewPadding,
          kCbzPreviewPadding, kCbzPreviewScreenWidth - 2 * kCbzPreviewPadding,
          kCbzPreviewScreenHeight - 2 * kCbzPreviewPadding);

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->SetColorMode(0);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  const char *top_source = has_interactive ? "interactive" : "preview";
  if (has_interactive) {
    BlitCbzCacheViewport(ts, ts->screenleft, kCbzZoomScreenHeight,
                         kCbzZoomScreenWidth, kCbzZoomScreenHeight,
                         cbz_state->current_interactive, viewport,
                         high_quality_viewport);
  } else {
    BlitCbzCacheViewport(ts, ts->screenleft, kCbzZoomScreenHeight,
                         kCbzZoomScreenWidth, kCbzZoomScreenHeight,
                         cbz_state->current_preview, viewport,
                         high_quality_viewport);
  }
  char top_msg[48];
  snprintf(top_msg, sizeof(top_msg), "CBZ %u/%u  %.1fx",
           (unsigned)(page_index + 1), (unsigned)cbz_state->page_count,
           pdf_view_utils::ZoomForIndex(cbz_state->zoom_index));
  ts->SetPen(12, 18);
  ts->PrintString(top_msg);

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  if (book->GetApp())
    book->GetApp()->DrawBottomGradientBackground();
  ts->FillRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kCbzPaper);
  if (CbzPreviewCacheValid(cbz_state->current_preview, page_index)) {
    BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, kCbzPreviewScreenHeight, preview_layout.x,
        preview_layout.y, preview_layout.width, preview_layout.height,
        cbz_state->current_preview.pixels, cbz_state->current_preview.bitmap_width,
        cbz_state->current_preview.bitmap_height, 0, 0,
        cbz_state->current_preview.bitmap_width,
        cbz_state->current_preview.bitmap_height, true);
  }
  ts->DrawRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kCbzFrame);

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
               (u16)(viewport_y + viewport_h), kCbzAccent);

  DBG_LOGF(book->GetApp(),
           "CBZ: draw page=%d source=%s zoom_index=%d final_pending=0 final=0 "
           "interactive=%d preview=%d inc=0/0 viewport=(%.3f,%.3f %.3fx%.3f)",
           page_index, top_source, cbz_state->zoom_index,
           has_interactive ? 1 : 0,
           CbzPreviewCacheValid(cbz_state->current_preview, page_index) ? 1 : 0,
           viewport.left, viewport.top, viewport.width, viewport.height);

  ts->SetStyle(saved_style);
  ts->SetColorMode(saved_color);
  ts->SetScreen(saved_screen);
  ts->margin.bottom = saved_bottom_margin;
}

void Book::SetFixedLayoutViewportInteraction(bool active) {
  if (IsPdf()) {
    SetMuPdfViewportInteraction(active);
    return;
  }
  if (IsCbz() && cbz_state)
    cbz_state->viewport_interaction_active = active;
}

bool Book::ChangeFixedLayoutZoom(int delta) {
  if (IsPdf())
    return ChangeMuPdfZoom(delta);
  if (!IsCbz() || !cbz_state || delta == 0)
    return false;

  const int next = std::min(
      cbz_state->max_zoom_index,
      pdf_view_utils::ClampZoomIndexForDevice(cbz_state->zoom_index + delta,
                                              cbz_state->is_new_3ds));
  if (next == cbz_state->zoom_index)
    return false;

  cbz_state->zoom_index = next;
  ResetCbzBitmapCache(&cbz_state->current_interactive);
  ResetCbzAdjacentSlot(&cbz_state->prev_slot);
  ResetCbzAdjacentSlot(&cbz_state->next_slot);
  cbz_state->preload_pending = false;
  DBG_LOGF(app, "CBZ: zoom page=%d zoom_index=%d final_pending=0",
           position, cbz_state->zoom_index);
  return true;
}

bool Book::MoveFixedLayoutViewportToPreview(int touch_x, int touch_y) {
  if (IsPdf())
    return MoveMuPdfViewportToPreview(touch_x, touch_y);
  if (!IsCbz() || !cbz_state)
    return false;

  const pdf_view_utils::PreviewLayout preview =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          cbz_state->page_width, cbz_state->page_height, kCbzPreviewPadding,
          kCbzPreviewPadding, kCbzPreviewScreenWidth - 2 * kCbzPreviewPadding,
          kCbzPreviewScreenHeight - 2 * kCbzPreviewPadding);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentCbzViewport(cbz_state);
  const pdf_view_utils::NormalizedPoint center =
      pdf_view_utils::RecenterViewportFromPreview(preview, viewport, touch_x,
                                                  touch_y);
  const float dx = std::abs(center.x - cbz_state->viewport_center_x);
  const float dy = std::abs(center.y - cbz_state->viewport_center_y);
  if (dx < 0.0005f && dy < 0.0005f)
    return false;

  cbz_state->viewport_center_x = center.x;
  cbz_state->viewport_center_y = center.y;
  DBG_LOGF(app,
           "CBZ: viewport_move page=%d touch=(%d,%d) center=(%.3f,%.3f) "
           "zoom_index=%d final_pending=0",
           position, touch_x, touch_y, center.x, center.y, cbz_state->zoom_index);
  return true;
}

bool Book::JumpFixedLayoutChapter(int delta) {
  if (IsPdf())
    return JumpMuPdfChapter(delta);
  (void)delta;
  return false;
}

bool Book::HasPendingFixedLayoutDeferredWork() const {
  if (IsPdf())
    return HasPendingMuPdfDeferredWork();
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return false;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  if (cbz_state->failed_page == page_index)
    return false;
  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index))
    return true;
  if (!CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->zoom_index)) {
    return true;
  }
  if (cbz_state->worker) {
    const bool job_pending =
        __atomic_load_n(&cbz_state->worker->job_pending, __ATOMIC_ACQUIRE);
    if (cbz_state->worker->job_submitted && !job_pending)
      return true;
    if (job_pending)
      return false;
  }
  return HasCbzNeighborPending(cbz_state, page_index);
}

u32 Book::GetFixedLayoutDeferredDelayMs() const {
  if (IsPdf())
    return GetMuPdfDeferredDelayMs();
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return 0;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index) ||
      !CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->zoom_index)) {
    return kCbzInteractiveDeferredDelayMs;
  }
  if (cbz_state->worker) {
    const bool job_pending =
        __atomic_load_n(&cbz_state->worker->job_pending, __ATOMIC_ACQUIRE);
    if (cbz_state->worker->job_submitted && !job_pending)
      return 0;
    if (job_pending)
      return 0;
  }
  return HasCbzNeighborPending(cbz_state, page_index)
             ? kCbzPreloadDeferredDelayMs
             : 0;
}

bool Book::PumpDeferredFixedLayoutWork(u32 budget_ms) {
  if (IsPdf())
    return PumpDeferredMuPdfWork(budget_ms);
  if (!IsCbz() || !cbz_state || cbz_state->page_count == 0)
    return false;

  const int page_index = ClampCbzPageIndex(position, cbz_state->page_count);
  const u64 start_ms = osGetTime();
  bool worked = false;

  if (!CbzPreviewCacheValid(cbz_state->current_preview, page_index)) {
    const u64 t0 = osGetTime();
    const bool preview_worked = EnsureCbzPreviewCache(cbz_state, page_index);
    worked = preview_worked || worked;
    DBG_LOGF(app, "CBZ: deferred preview page=%d ms=%llu worked=%d",
             page_index, (unsigned long long)(osGetTime() - t0),
             preview_worked ? 1 : 0);
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (!CbzBitmapCacheValid(cbz_state->current_interactive, page_index,
                           cbz_state->zoom_index)) {
    const u64 t0 = osGetTime();
    if (EnsureCbzInteractiveCache(cbz_state, page_index))
      worked = true;
    DBG_LOGF(app, "CBZ: deferred interactive page=%d ms=%llu worked=%d",
             page_index, (unsigned long long)(osGetTime() - t0),
             worked ? 1 : 0);
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  const u64 t_preload = osGetTime();
  const CbzPreloadPumpResult preload_result =
      PumpCbzPreloadWorker(cbz_state, page_index);
  if (preload_result != CbzPreloadPumpResult::Idle) {
    const char *stage =
        (preload_result == CbzPreloadPumpResult::Submitted) ? "submitted"
                                                            : "integrated";
    DBG_LOGF(app, "CBZ: deferred next page=%d ms=%llu stage=%s", page_index,
             (unsigned long long)(osGetTime() - t_preload), stage);
  }
  return worked || preload_result == CbzPreloadPumpResult::Integrated;
}

void Book::CancelFixedLayoutDeferredWork() {
  if (IsPdf()) {
    CancelMuPdfIncrementalRender();
    return;
  }
  if (!IsCbz() || !cbz_state)
    return;
  cbz_state->preload_pending = false;
}
