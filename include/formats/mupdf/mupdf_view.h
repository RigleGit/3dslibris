#pragma once

#include "formats/mupdf/mupdf_common.h"
#include "formats/mupdf/mupdf_worker.h"
#include "formats/common/pdf_view_utils.h"

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
