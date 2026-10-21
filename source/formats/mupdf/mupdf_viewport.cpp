// SPDX-License-Identifier: AGPL-3.0-or-later
// Viewport state management (pan, zoom, chapter navigation) for MuPDF views.
// Render pipeline and blit helpers are in mupdf_render.cpp.

#include "formats/mupdf/mupdf_view.h"

#include "book/page.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "settings/prefs.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "ui/text.h"

namespace {

static void ResetMuPdfDeferredCachesForSynchronousRender(
    Book::MuPdfState *mupdf_state) {
  if (!mupdf_state)
    return;
  CancelMuPdfIncrementalRenderState(mupdf_state);
  ResetBitmapCache(&mupdf_state->current_interactive_tile);
  ResetBitmapCache(&mupdf_state->current_final_zoom);
  // Zoom changes raster dimensions, not the page's drawing commands.
  // Keep the list; EnsureMuPdfDisplayListForPage invalidates it on page change.
  if (mupdf_state->ctx) {
    ResetAdjacentSlot(&mupdf_state->prev_slot, mupdf_state->ctx);
    ResetAdjacentSlot(&mupdf_state->next_slot, mupdf_state->ctx);
  }
  ResetMuPdfRenderFailureState(mupdf_state);
  mupdf_state->final_cache_pending = false;
}

} // namespace

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
          (float)kPdfZoomScreenWidth, (float)kPdfZoomScreenHeight,
          local_center_x, local_center_y);

  out.left = nav.left + local.left * nav.width;
  out.top = nav.top + local.top * nav.height;
  out.width = local.width * nav.width;
  out.height = local.height * nav.height;
  return out;
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

static void GetMuPdfPreviewContentBounds(const Book::MuPdfState *mupdf_state,
                                         float *left, float *top,
                                         float *width, float *height) {
  if (!left || !top || !width || !height)
    return;
  if (mupdf_state && BitmapCacheValid(mupdf_state->current_preview,
                                      mupdf_state->current_preview.page)) {
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
  const MuPdfNavigationBounds nav =
      GetMuPdfNavigationBounds(left, top, width, height);
  const float local_center_x =
      std::max(0.0f, std::min(1.0f,
          (mupdf_state->viewport.center_x - nav.left) /
              std::max(0.0001f, nav.width)));
  const float local_center_y =
      std::max(0.0f, std::min(1.0f,
          (mupdf_state->viewport.center_y - nav.top) /
              std::max(0.0001f, nav.height)));
  const pdf_view_utils::NormalizedRect local =
      pdf_view_utils::ComputeViewportRect(
          std::max(1.0f, mupdf_state->page_width * nav.width),
          std::max(1.0f, mupdf_state->page_height * nav.height),
          ComputeEffectiveMuPdfZoom(mupdf_state->document_kind,
                                    mupdf_state->viewport.zoom_index),
          (float)mupdf_state->target_top_width,
          (float)mupdf_state->target_top_height, local_center_x,
          local_center_y);
  pdf_view_utils::NormalizedRect out;
  out.left = nav.left + local.left * nav.width;
  out.top = nav.top + local.top * nav.height;
  out.width = local.width * nav.width;
  out.height = local.height * nav.height;
  return out;
}

bool Book::ChangeMuPdfZoom(int delta) {
  if (!IsPdf() || !mupdf_state || delta == 0)
    return false;
  const int next = std::min(
      mupdf_state->viewport.max_zoom_index,
      pdf_view_utils::ClampInteractiveZoomIndexForDevice(
          mupdf_state->viewport.zoom_index + delta, mupdf_state->is_new_3ds));
  if (next == mupdf_state->viewport.zoom_index)
    return false;
  mupdf_state->viewport.zoom_index = next;
  if (debug_runtime::ForceSynchronousMuPdfRender()) {
    ResetMuPdfDeferredCachesForSynchronousRender(mupdf_state);
    return true;
  }
  if (app_flow_utils::MuPdfWantsFinalQualityRender(
          mupdf_state->document_kind) &&
      (mupdf_state->current_final_zoom.page != position ||
       mupdf_state->current_final_zoom.zoom_index <
           mupdf_state->viewport.max_zoom_index)) {
    mupdf_state->final_cache_pending = true;
    CancelMuPdfIncrementalRenderState(mupdf_state);
  } else if (!app_flow_utils::MuPdfWantsFinalQualityRender(
                 mupdf_state->document_kind)) {
    mupdf_state->final_cache_pending = false;
  }
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
          mupdf_state->target_bottom_width - 2 * kPdfPreviewPadding,
          mupdf_state->target_bottom_height - 2 * kPdfPreviewPadding);
  const MuPdfNavigationBounds nav =
      GetMuPdfNavigationBounds(mupdf_state->current_preview.left,
                               mupdf_state->current_preview.top,
                               mupdf_state->current_preview.width,
                               mupdf_state->current_preview.height);
  const pdf_view_utils::NormalizedRect viewport =
      ComputeCurrentMuPdfViewport(mupdf_state);
  const pdf_view_utils::NormalizedPoint center =
      RecenterMuPdfViewportFromPreview(preview, viewport, nav, touch_x, touch_y);
  const float dx = std::abs(center.x - mupdf_state->viewport.center_x);
  const float dy = std::abs(center.y - mupdf_state->viewport.center_y);
  if (dx < 0.0005f && dy < 0.0005f)
    return false;
  mupdf_state->viewport.center_x = center.x;
  mupdf_state->viewport.center_y = center.y;
  return true;
}

bool Book::TranslateMuPdfViewport(float dx, float dy) {
  if (!IsPdf() || !mupdf_state)
    return false;
  const float new_x =
      std::max(0.0f, std::min(1.0f, mupdf_state->viewport.center_x + dx));
  const float new_y =
      std::max(0.0f, std::min(1.0f, mupdf_state->viewport.center_y + dy));
  if (std::abs(new_x - mupdf_state->viewport.center_x) < 0.0005f &&
      std::abs(new_y - mupdf_state->viewport.center_y) < 0.0005f)
    return false;
  mupdf_state->viewport.center_x = new_x;
  mupdf_state->viewport.center_y = new_y;
  return true;
}

void Book::SetMuPdfViewportInteraction(bool active) {
  if (!IsPdf() || !mupdf_state)
    return;
  mupdf_state->viewport.interaction_active = active;
}

void Book::ResetMuPdfViewport() {
  if (!IsPdf() || !mupdf_state)
    return;
  const fixed_layout_viewport_utils::PageTurnDirection direction =
      (GetPrefs() && GetPrefs()->fixed_layout_rtl)
          ? fixed_layout_viewport_utils::PAGE_TURN_RIGHT_TO_LEFT
          : fixed_layout_viewport_utils::PAGE_TURN_LEFT_TO_RIGHT;
  const fixed_layout_viewport_utils::ViewportCenter center =
      fixed_layout_viewport_utils::DefaultPageTurnViewportCenter(direction);
  mupdf_state->viewport.center_x = center.x;
  mupdf_state->viewport.center_y = center.y;
  mupdf_state->viewport.interaction_active = false;
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
