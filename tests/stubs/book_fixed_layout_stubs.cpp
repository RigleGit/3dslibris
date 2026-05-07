/*
 * Stubs for CBZ/MuPDF Book methods (cbz_document, cbz_view,
 * mupdf_document, mupdf_view, cbz_worker, mupdf_worker).
 * All return safe defaults; the TXT/FB2 parser paths never call these.
 */
#include "book/book.h"
#include "formats/cbz/cbz_types.h"

// ---- CBZ ----

void Book::ResetCbzState() {
  if (!cbz_state)
    return;
  delete cbz_state;
  cbz_state = nullptr;
}

void Book::ResetCbzTransientViewState(bool) {}

void Book::DrawCurrentCbzView(Text *) {}
void Book::SetCbzViewportInteraction(bool) {}
void Book::ResetCbzViewport() {}
bool Book::ChangeCbzZoom(int) { return false; }
bool Book::MoveCbzViewportToPreview(int, int) { return false; }
bool Book::TranslateCbzViewport(float, float) { return false; }
bool Book::JumpCbzChapter(int) { return false; }
bool Book::HasPendingCbzDeferredWork() const { return false; }
u32 Book::GetCbzDeferredDelayMs() const { return 0; }

void Book::InitCbzView(const std::string &, const std::vector<CbzPageEntry> &,
                       bool) {}

void Book::InitMuPdfView(u16, fz_context *, fz_document *, fz_outline *,
                         bool, app_flow_utils::MuPdfDocumentKind) {}

// ---- MuPDF ----

void Book::ResetMuPdfState() {
  if (!mupdf_state)
    return;
  delete mupdf_state;
  mupdf_state = nullptr;
}

bool Book::ChangeMuPdfZoom(int) { return false; }
bool Book::MoveMuPdfViewportToPreview(int, int) { return false; }
bool Book::TranslateMuPdfViewport(float, float) { return false; }
void Book::SetMuPdfViewportInteraction(bool) {}
void Book::ResetMuPdfViewport() {}
bool Book::JumpMuPdfChapter(int) { return false; }
bool Book::HasPendingMuPdfDeferredWork() const { return false; }
void Book::CancelMuPdfIncrementalRender() {}
u32 Book::GetMuPdfDeferredDelayMs() const { return 0; }
void Book::DrawCurrentMuPdfView(Text *) {}

bool Book::PumpDeferredCbzWork(u32) { return false; }
void Book::CancelCbzDeferredWork() {}
bool Book::PumpDeferredMuPdfWork(u32) { return false; }
