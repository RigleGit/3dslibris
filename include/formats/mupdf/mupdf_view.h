#pragma once

#include "formats/mupdf/mupdf_common.h"
#include "formats/mupdf/mupdf_worker.h"
#include "formats/common/pdf_view_utils.h"

// True when the cached interactive tile of `page_index` spans `viewport`.
bool MuPdfInteractiveTileCoversViewport(
    const Book::MuPdfState *state, int page_index,
    const pdf_view_utils::NormalizedRect &viewport);
bool CanReuseMuPdfInteractiveRaster(
    const Book::MuPdfState *state, int page_index,
    const pdf_view_utils::NormalizedRect &viewport);

MuPdfNavigationBounds GetMuPdfNavigationBounds(float content_left,
                                               float content_top,
                                               float content_width,
                                               float content_height);
pdf_view_utils::NormalizedRect ComputeMuPdfViewportRect(
    float page_width, float page_height,
    app_flow_utils::MuPdfDocumentKind document_kind, int zoom_index,
    float center_x, float center_y, float content_left, float content_top,
    float content_width, float content_height);
int ClampMuPdfPageIndex(int page_index, u16 page_count);
MuPdfDeferredStage GetNextMuPdfDeferredStage(
    const Book::MuPdfState *mupdf_state, int page_index,
    const pdf_view_utils::NormalizedRect &viewport);
pdf_view_utils::NormalizedRect ComputeCurrentMuPdfViewport(
    const Book::MuPdfState *mupdf_state);

// Zoomed-PDF tile mosaic (source/formats/mupdf/mupdf_mosaic.cpp).
// True when the current page/zoom is served by fixed GPU tiles rather than one
// interactive raster.
bool UseMuPdfMosaic(const Book::MuPdfState *state);
// Tiles are still missing and the idle pump should keep redrawing.
bool MuPdfMosaicWantsIdleWork(const Book::MuPdfState *state);
// Renders missing tiles (time-budgeted) and queues the frame. False when
// nothing could be queued; the caller then falls back to the classic path.
bool DrawMuPdfMosaic(Book::MuPdfState *state, Text *text, int page_index,
                     const pdf_view_utils::NormalizedRect &viewport,
                     const pdf_view_utils::PreviewLayout &top_layout);
