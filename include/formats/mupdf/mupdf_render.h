#pragma once

#include "formats/mupdf/mupdf_common.h"
#include "formats/common/pdf_view_utils.h"

class IStatusReporter;

fz_matrix MakeMuPdfRenderMatrix(fz_rect page_bounds, float scale);
bool QueryMuPdfPageMetrics(fz_context *ctx, fz_document *doc, int page_index,
                           float *page_width, float *page_height);
bool RenderMuPdfBitmap(
    fz_context *ctx, fz_document *doc, int page_index, float scale,
    RenderedMuPdfBitmap *out, float *page_width, float *page_height,
    const pdf_view_utils::NormalizedRect *crop_rect = NULL,
    fz_display_list *reuse_list = NULL, fz_display_list **out_list = NULL,
    IStatusReporter *reporter = NULL);
void AddMuPdfOutlineEntries(Book *book, fz_context *ctx, fz_document *doc,
                            const fz_outline *entry, u8 level);
void PopulateMuPdfMetadata(Book *book, fz_context *ctx, fz_document *doc);
float ComputeMuPdfPreviewScale(float page_width, float page_height);
float ComputeMuPdfFinalScale(app_flow_utils::MuPdfDocumentKind document_kind,
                             float page_width, float page_height,
                             int max_zoom_index);
void ComputeBitmapContentBoundsNormalized(const RenderedMuPdfBitmap &bitmap,
                                          float *left, float *top,
                                          float *width, float *height);
