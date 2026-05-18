#pragma once

#include "formats/mupdf/mupdf_common.h"
#include "formats/mupdf/mupdf_render.h"
#include "formats/common/pdf_view_utils.h"

void CancelMuPdfIncrementalRenderState(Book::MuPdfState *mupdf_state);
bool PromoteMuPdfAdjacentSlotIfMatching(Book::MuPdfState *mupdf_state,
                                        int page_index);
bool EnsureMuPdfDisplayListForPage(Book::MuPdfState *mupdf_state,
                                   int page_index,
                                   fz_display_list **out_list);
bool EnsureCurrentMuPdfPreviewCache(Book::MuPdfState *mupdf_state,
                                    int page_index);
bool EnsureCurrentMuPdfInteractiveTile(Book::MuPdfState *mupdf_state,
                                       int page_index);
void InitMuPdfWorker(Book::MuPdfState *mupdf_state);
void SignalMuPdfWorkerShutdown(Book::MuPdfState *mupdf_state);
void ShutdownMuPdfWorker(Book::MuPdfState *mupdf_state);
bool PumpMuPdfIncrementalStrip(Book::MuPdfState *mupdf_state, int page_index);
bool PrepareAdjacentMuPdfSlot(Book::MuPdfState *mupdf_state, int current_page,
                              int direction);
