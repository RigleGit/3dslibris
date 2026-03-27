// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_view.h"

#include "app/app.h"
#include "book/page.h"
#include "ui/text.h"
#include "debug_log.h"

MuPdfNavigationBounds GetMuPdfNavigationBounds(float content_left,
                                               float content_top,
                                               float content_width,
                                               float content_height) {
  MuPdfNavigationBounds bounds;
  (void)content_left;
  (void)content_top;
  (void)content_width;
  (void)content_height;
  return bounds;
}

pdf_view_utils::NormalizedRect ComputeMuPdfViewportRect(
    float page_width, float page_height,
    app_flow_utils::MuPdfDocumentKind document_kind, int zoom_index,
    float center_x, float center_y, float content_left, float content_top,
    float content_width, float content_height) {
  pdf_view_utils::NormalizedRect out = {0.0f, 0.0f, 1.0f, 1.0f};
  const MuPdfNavigationBounds nav =
      GetMuPdfNavigationBounds(content_left, content_top, content_width,
                               content_height);
  const float local_center_x =
      std::max(0.0f, std::min(1.0f, (center_x - nav.left) /
                                        std::max(0.0001f, nav.width)));
  const float local_center_y =
      std::max(0.0f, std::min(1.0f, (center_y - nav.top) /
                                        std::max(0.0001f, nav.height)));
  const pdf_view_utils::NormalizedRect local =
      pdf_view_utils::ComputeViewportRect(
          std::max(1.0f, page_width * nav.width),
          std::max(1.0f, page_height * nav.height),
          ComputeEffectiveMuPdfZoom(document_kind, zoom_index),
          (float)kPdfZoomScreenWidth,
          (float)kPdfZoomScreenHeight, local_center_x, local_center_y);

  out.left = nav.left + local.left * nav.width;
  out.top = nav.top + local.top * nav.height;
  out.width = local.width * nav.width;
  out.height = local.height * nav.height;
  return out;
}

int ClampMuPdfPageIndex(int page_index, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (page_index < 0)
    return 0;
  if (page_index >= (int)page_count)
    return (int)page_count - 1;
  return page_index;
}

static void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                       int x, int y, int draw_width,
                                       int draw_height,
                                       const std::vector<u16> &pixels,
                                       int src_width, int src_height,
                                       int crop_x, int crop_y, int crop_width,
                                       int crop_height,
                                       bool high_quality_filter) {
  if (!ts || !screen || pixels.empty() || draw_width <= 0 || draw_height <= 0 ||
      src_width <= 0 || src_height <= 0 || crop_width <= 0 || crop_height <= 0) {
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

      int r00, g00, b00;
      int r10, g10, b10;
      int r01, g01, b01;
      int r11, g11, b11;
      RGB565ToRgb8(pixels[(size_t)y0 * (size_t)src_width + (size_t)x0], &r00, &g00, &b00);
      RGB565ToRgb8(pixels[(size_t)y0 * (size_t)src_width + (size_t)x1], &r10, &g10, &b10);
      RGB565ToRgb8(pixels[(size_t)y1 * (size_t)src_width + (size_t)x0], &r01, &g01, &b01);
      RGB565ToRgb8(pixels[(size_t)y1 * (size_t)src_width + (size_t)x1], &r11, &g11, &b11);

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;

      const unsigned char r = (unsigned char)(
          r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 0.5f);
      const unsigned char g = (unsigned char)(
          g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 0.5f);
      const unsigned char b = (unsigned char)(
          b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 0.5f);

      screen[(size_t)dy * (size_t)stride + (size_t)dx] =
          RGB565FromRgb8(r, g, b);
    }
  }
}

static pdf_view_utils::NormalizedPoint RecenterMuPdfViewportFromPreview(
    const pdf_view_utils::PreviewLayout &preview,
    const pdf_view_utils::NormalizedRect &viewport,
    const MuPdfNavigationBounds &nav, int touch_x, int touch_y) {
  pdf_view_utils::NormalizedPoint out = {0.5f, 0.5f};
  if (preview.width <= 0 || preview.height <= 0)
    return out;

  float px = (float)(touch_x - preview.x) / (float)preview.width;
  float py = (float)(touch_y - preview.y) / (float)preview.height;
  px = std::max(0.0f, std::min(1.0f, px));
  py = std::max(0.0f, std::min(1.0f, py));

  const float min_x = nav.left + viewport.width * 0.5f;
  const float max_x = nav.left + nav.width - viewport.width * 0.5f;
  const float min_y = nav.top + viewport.height * 0.5f;
  const float max_y = nav.top + nav.height - viewport.height * 0.5f;
  out.x = std::max(min_x, std::min(max_x, px));
  out.y = std::max(min_y, std::min(max_y, py));
  return out;
}

static void GetMuPdfPreviewContentBounds(const Book::MuPdfState *mupdf_state, float *left,
                                    float *top, float *width, float *height) {
  if (!left || !top || !width || !height)
    return;
  if (mupdf_state && BitmapCacheValid(mupdf_state->current_preview, mupdf_state->current_preview.page)) {
    *left = mupdf_state->current_preview.left;
    *top = mupdf_state->current_preview.top;
    *width = mupdf_state->current_preview.width;
    *height = mupdf_state->current_preview.height;
    return;
  }
  *left = 0.0f;
  *top = 0.0f;
  *width = 1.0f;
  *height = 1.0f;
}

pdf_view_utils::NormalizedRect ComputeCurrentMuPdfViewport(
    const Book::MuPdfState *mupdf_state) {
  float left = 0.0f;
  float top = 0.0f;
  float width = 1.0f;
  float height = 1.0f;
  GetMuPdfPreviewContentBounds(mupdf_state, &left, &top, &width, &height);
  return ComputeMuPdfViewportRect(
      mupdf_state->page_width, mupdf_state->page_height,
      mupdf_state->document_kind, mupdf_state->zoom_index,
      mupdf_state->viewport_center_x, mupdf_state->viewport_center_y, left,
      top, width, height);
}

MuPdfDeferredStage GetNextMuPdfDeferredStage(
    const Book::MuPdfState *mupdf_state, int page_index,
    const pdf_view_utils::NormalizedRect &viewport) {
  if (!mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return MuPdfDeferredStage::None;

  if (!BitmapCacheValid(mupdf_state->current_interactive_tile, page_index)) {
    return MuPdfDeferredStage::Interactive;
  }

  if (app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind) &&
      (mupdf_state->final_cache_pending ||
       !BitmapCacheValid(mupdf_state->current_final_zoom, page_index))) {
    return MuPdfDeferredStage::Final;
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

  BlitRgb565BitmapScaledCrop(ts, screen, logical_height, 0, 0, draw_width,
                             draw_height, cache.pixels, cache.bitmap_width,
                             cache.bitmap_height, crop_x, crop_y, crop_w,
                             crop_h, high_quality_filter);
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

  BlitRgb565BitmapScaledCrop(ts, screen, logical_height, 0, dst_y0, draw_width,
                             dst_y1 - dst_y0, cache.pixels,
                             cache.bitmap_width, cache.bitmap_height,
                             crop_x, crop_y, crop_w, crop_h,
                             high_quality_filter);
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

  BlitRgb565BitmapScaledCrop(ts, screen, logical_height, 0, dst_y0, draw_width,
                             dst_y1 - dst_y0, pixels, bitmap_width,
                             bitmap_height, crop_x, crop_y, crop_w, crop_h,
                             false);
  return true;
}

bool Book::ChangeMuPdfZoom(int delta) {
  if (!IsPdf() || !mupdf_state || delta == 0)
    return false;
  const int next = std::min(
      mupdf_state->max_zoom_index,
      pdf_view_utils::ClampZoomIndexForDevice(mupdf_state->zoom_index + delta,
                                              mupdf_state->is_new_3ds));
  if (next == mupdf_state->zoom_index)
    return false;
  mupdf_state->zoom_index = next;
  if (app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind) &&
      (mupdf_state->current_final_zoom.page != position ||
       mupdf_state->current_final_zoom.zoom_index <
           mupdf_state->max_zoom_index)) {
    mupdf_state->final_cache_pending = true;
    CancelMuPdfIncrementalRenderState(mupdf_state);
  } else if (!app_flow_utils::MuPdfWantsFinalQualityRender(
                 mupdf_state->document_kind)) {
    mupdf_state->final_cache_pending = false;
  }
  DBG_LOGF(app, "%s: zoom page=%d zoom_index=%d final_pending=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           position, mupdf_state->zoom_index,
           mupdf_state->final_cache_pending ? 1 : 0);
  return true;
}

bool Book::MoveMuPdfViewportToPreview(int touch_x, int touch_y) {
  if (!IsPdf() || !mupdf_state)
    return false;
  float preview_source_width = mupdf_state->page_width;
  float preview_source_height = mupdf_state->page_height;
  if (mupdf_state->current_preview.bitmap_width > 0 &&
      mupdf_state->current_preview.bitmap_height > 0) {
    preview_source_width = (float)mupdf_state->current_preview.bitmap_width;
    preview_source_height = (float)mupdf_state->current_preview.bitmap_height;
  }
  const pdf_view_utils::PreviewLayout preview =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height,
          kPdfPreviewPadding, kPdfPreviewPadding,
          kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
          kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);
  const MuPdfNavigationBounds nav =
      GetMuPdfNavigationBounds(mupdf_state->current_preview.left,
                               mupdf_state->current_preview.top,
                               mupdf_state->current_preview.width,
                               mupdf_state->current_preview.height);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentMuPdfViewport(mupdf_state);
  const pdf_view_utils::NormalizedPoint center =
      RecenterMuPdfViewportFromPreview(preview, viewport, nav, touch_x, touch_y);
  const float dx = std::abs(center.x - mupdf_state->viewport_center_x);
  const float dy = std::abs(center.y - mupdf_state->viewport_center_y);
  if (dx < 0.0005f && dy < 0.0005f)
    return false;
  mupdf_state->viewport_center_x = center.x;
  mupdf_state->viewport_center_y = center.y;
  DBG_LOGF(app,
           "%s: viewport_move page=%d touch=(%d,%d) center=(%.3f,%.3f) "
           "zoom_index=%d final_pending=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           position, touch_x, touch_y, center.x, center.y,
           mupdf_state->zoom_index, mupdf_state->final_cache_pending ? 1 : 0);
  return true;
}

void Book::SetMuPdfViewportInteraction(bool active) {
  if (!IsPdf() || !mupdf_state)
    return;
  mupdf_state->viewport_interaction_active = active;
}

bool Book::JumpMuPdfChapter(int delta) {
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

bool Book::HasPendingMuPdfDeferredWork() const {
  if (!IsPdf() || !mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
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

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentMuPdfViewport(mupdf_state);

  switch (GetNextMuPdfDeferredStage(mupdf_state, page_index, viewport)) {
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

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  position = page_index;

  if (mupdf_state->current_preview.page != page_index)
    ResetBitmapCache(&mupdf_state->current_preview);
  if (mupdf_state->current_interactive_tile.page != page_index)
    ResetBitmapCache(&mupdf_state->current_interactive_tile);
  if (mupdf_state->current_final_zoom.page != page_index)
    ResetBitmapCache(&mupdf_state->current_final_zoom);

  float page_width = mupdf_state->page_width;
  float page_height = mupdf_state->page_height;
  if (QueryMuPdfPageMetrics(mupdf_state->ctx, mupdf_state->doc, page_index,
                          &page_width, &page_height)) {
    mupdf_state->page_width = page_width;
    mupdf_state->page_height = page_height;
  }

  if (!EnsureCurrentMuPdfPreviewCache(mupdf_state, page_index))
    return;

  pdf_view_utils::NormalizedRect viewport = ComputeCurrentMuPdfViewport(mupdf_state);
  mupdf_state->viewport_center_x = viewport.left + viewport.width * 0.5f;
  mupdf_state->viewport_center_y = viewport.top + viewport.height * 0.5f;
  const bool has_final_cache =
      BitmapCacheValid(mupdf_state->current_final_zoom, page_index) &&
      mupdf_state->current_final_zoom.zoom_index >= mupdf_state->max_zoom_index;
  const bool has_interactive_tile =
      BitmapCacheValid(mupdf_state->current_interactive_tile, page_index);
  const bool wants_final_cache =
      app_flow_utils::MuPdfWantsFinalQualityRender(mupdf_state->document_kind);
  mupdf_state->final_cache_pending = wants_final_cache && !has_final_cache;
  const bool high_quality_viewport =
      !mupdf_state->viewport_interaction_active;
#ifdef DSLIBRIS_DEBUG
  const char *top_source = "none";
#endif
  const float preview_source_width =
      std::max(1.0f, (float)mupdf_state->current_preview.bitmap_width);
  const float preview_source_height =
      std::max(1.0f, (float)mupdf_state->current_preview.bitmap_height);
  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayoutInBounds(
          preview_source_width, preview_source_height, kPdfPreviewPadding,
          kPdfPreviewPadding, kPdfPreviewScreenWidth - 2 * kPdfPreviewPadding,
          kPdfPreviewScreenHeight - 2 * kPdfPreviewPadding);

  const int saved_style = ts->GetStyle();
  const int saved_color = ts->GetColorMode();
  u16 *saved_screen = ts->GetScreen();
  const int saved_bottom_margin = ts->margin.bottom;

  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->SetColorMode(0);
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();

  if (has_final_cache) {
#ifdef DSLIBRIS_DEBUG
    top_source = "final";
#endif
    BlitBitmapCacheViewport(ts, ts->screenleft, kPdfZoomScreenHeight,
                            kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                            mupdf_state->current_final_zoom, viewport,
                            high_quality_viewport);
  } else if (mupdf_state->incremental.active &&
             mupdf_state->incremental.strips_completed > 0 &&
             mupdf_state->incremental.target_page == page_index) {
#ifdef DSLIBRIS_DEBUG
    top_source = "incremental";
#endif
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
        ? std::min(kPdfZoomScreenHeight,
                   (int)(rendered_in_vp / vp_h * kPdfZoomScreenHeight + 0.5f))
        : 0;

    if (split_y > 0) {
      BlitRawBitmapViewportRegion(ts, ts->screenleft, kPdfZoomScreenHeight,
                                  kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                                  0, split_y, inc.partial_pixels,
                                  inc.partial_width, inc.partial_height,
                                  0.0f, 0.0f, 1.0f, 1.0f, viewport);
    }
    if (split_y < kPdfZoomScreenHeight) {
      if (has_interactive_tile) {
        BlitBitmapCacheViewportRegion(ts, ts->screenleft, kPdfZoomScreenHeight,
                                      kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                                      split_y, kPdfZoomScreenHeight,
                                      mupdf_state->current_interactive_tile,
                                      viewport, high_quality_viewport);
      } else if (BitmapCacheValid(mupdf_state->current_preview, page_index)) {
        BlitBitmapCacheViewportRegion(ts, ts->screenleft, kPdfZoomScreenHeight,
                                      kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                                      split_y, kPdfZoomScreenHeight,
                                      mupdf_state->current_preview, viewport,
                                      high_quality_viewport);
      }
    }
  } else if (has_interactive_tile) {
#ifdef DSLIBRIS_DEBUG
    top_source = "interactive";
#endif
    BlitBitmapCacheViewport(ts, ts->screenleft, kPdfZoomScreenHeight,
                            kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                            mupdf_state->current_interactive_tile, viewport,
                            high_quality_viewport);
  } else if (BitmapCacheValid(mupdf_state->current_preview, page_index)) {
#ifdef DSLIBRIS_DEBUG
    top_source = "preview";
#endif
    BlitBitmapCacheViewport(ts, ts->screenleft, kPdfZoomScreenHeight,
                            kPdfZoomScreenWidth, kPdfZoomScreenHeight,
                            mupdf_state->current_preview, viewport,
                            high_quality_viewport);
  } else {
    ts->SetPen(18, 28);
    ts->PrintString("MuPDF render unavailable");
  }
  char top_msg[48];
  snprintf(top_msg, sizeof(top_msg), "%s %u/%u  %.1fx",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           (unsigned)(page_index + 1), (unsigned)mupdf_state->page_count,
           pdf_view_utils::ZoomForIndex(mupdf_state->zoom_index));
  ts->SetPen(12, 18);
  ts->PrintString(top_msg);

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  if (app)
    app->DrawBottomGradientBackground();
  ts->FillRect((u16)preview_layout.x, (u16)preview_layout.y,
               (u16)(preview_layout.x + preview_layout.width),
               (u16)(preview_layout.y + preview_layout.height), kPdfPaper);

  if (!mupdf_state->current_preview.pixels.empty() &&
      mupdf_state->current_preview.bitmap_width > 0 &&
      mupdf_state->current_preview.bitmap_height > 0) {
    BlitRgb565BitmapScaledCrop(
        ts, ts->screenright, kPdfPreviewScreenHeight, preview_layout.x,
        preview_layout.y, preview_layout.width, preview_layout.height,
        mupdf_state->current_preview.pixels,
        mupdf_state->current_preview.bitmap_width,
        mupdf_state->current_preview.bitmap_height, 0, 0,
        mupdf_state->current_preview.bitmap_width,
        mupdf_state->current_preview.bitmap_height, true);
  }
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
  DBG_LOGF(app,
           "%s: draw page=%d source=%s zoom_index=%d final_pending=%d "
           "final=%d interactive=%d preview=%d inc=%d/%d "
           "viewport=(%.3f,%.3f %.3fx%.3f)",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           page_index, top_source, mupdf_state->zoom_index,
           mupdf_state->final_cache_pending ? 1 : 0,
           has_final_cache ? 1 : 0, has_interactive_tile ? 1 : 0,
           BitmapCacheValid(mupdf_state->current_preview, page_index) ? 1 : 0,
           (mupdf_state->incremental.active && mupdf_state->incremental.target_page == page_index)
               ? mupdf_state->incremental.strips_completed : 0,
           (mupdf_state->incremental.active && mupdf_state->incremental.target_page == page_index)
               ? mupdf_state->incremental.strips_total : 0,
           viewport.left, viewport.top, viewport.width, viewport.height);

  ts->SetStyle(saved_style);
  ts->SetColorMode(saved_color);
  ts->SetScreen(saved_screen);
  ts->margin.bottom = saved_bottom_margin;
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
  if (!IsPdf() || !mupdf_state || !mupdf_state->ctx || !mupdf_state->doc)
    return false;

  const int page_index = ClampMuPdfPageIndex(position, mupdf_state->page_count);
  const u64 start_ms = osGetTime();
  bool worked = false;
  if (!HasPendingMuPdfDeferredWork())
    return false;

  DBG_LOGF(app,
           "%s: deferred start page=%d budget=%u zoom_index=%d final_pending=%d "
           "have_final=%d have_interactive=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           page_index, (unsigned)budget_ms, mupdf_state->zoom_index,
           mupdf_state->final_cache_pending ? 1 : 0,
           BitmapCacheValid(mupdf_state->current_final_zoom, page_index) ? 1 : 0,
           BitmapCacheValid(mupdf_state->current_interactive_tile, page_index)
               ? 1
               : 0);

  if (!BitmapCacheValid(mupdf_state->current_interactive_tile, page_index)) {
#ifdef DSLIBRIS_DEBUG
    const u64 t0 = osGetTime();
#endif
    if (EnsureCurrentMuPdfInteractiveTile(mupdf_state, page_index))
      worked = true;
    DBG_LOGF(app,
             "%s: deferred interactive page=%d ms=%llu worked=%d",
             app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
             page_index, (unsigned long long)(osGetTime() - t0),
             worked ? 1 : 0);
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

  if (mupdf_state->final_cache_pending ||
      !BitmapCacheValid(mupdf_state->current_final_zoom, page_index) ||
      mupdf_state->incremental.active) {
#ifdef DSLIBRIS_DEBUG
    const u64 t0 = osGetTime();
    const int pre_strips = mupdf_state->incremental.strips_completed;
    const int pre_total = mupdf_state->incremental.strips_total;
#endif
    if (PumpMuPdfIncrementalStrip(mupdf_state, page_index))
      worked = true;
    DBG_LOGF(app,
             "%s: deferred strip page=%d strip=%d/%d ms=%llu worked=%d",
             app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
             page_index, pre_strips, pre_total,
             (unsigned long long)(osGetTime() - t0), worked ? 1 : 0);
    if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
      return worked;
  }

#ifdef DSLIBRIS_DEBUG
  const u64 t_next = osGetTime();
#endif
  if (PrepareAdjacentMuPdfSlot(mupdf_state, page_index, 1))
    worked = true;
  DBG_LOGF(app, "%s: deferred next page=%d ms=%llu worked=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           page_index,
           (unsigned long long)(osGetTime() - t_next), worked ? 1 : 0);
  if (budget_ms > 0 && osGetTime() - start_ms >= budget_ms)
    return worked;

#ifdef DSLIBRIS_DEBUG
  const u64 t_prev = osGetTime();
#endif
  if (PrepareAdjacentMuPdfSlot(mupdf_state, page_index, -1))
    worked = true;
  DBG_LOGF(app, "%s: deferred prev page=%d ms=%llu worked=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           page_index,
           (unsigned long long)(osGetTime() - t_prev), worked ? 1 : 0);
  DBG_LOGF(app, "%s: deferred end page=%d total_ms=%llu worked=%d",
           app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind),
           page_index, (unsigned long long)(osGetTime() - start_ms),
           worked ? 1 : 0);

  return worked;
}
