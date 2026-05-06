#include "book/book.h"

#include "book/page.h"
#include "formats/mupdf/mupdf_worker.h"
#include "formats/cbz/cbz_worker.h"
#include "shared/debug_log.h"

void Book::SuspendFixedLayoutWorkers() {
  // Stop background worker threads before the HOME menu takes over.
  // Workers are New3DS-only threadCreate threads on core 1. Leaving them alive
  // across an APT suspend can leave the system in a bad state and cause the
  // HOME menu to panic (svcBreak). ResumeFixedLayoutWorkers() restarts them.
  //
  // For MuPDF: ShutdownMuPdfWorker joins the thread and NULLs worker, but
  // leaves the fz_context/fz_document intact. We also reset worker_init_attempted
  // so that ResumeFixedLayoutWorkers() can call InitMuPdfWorker() again.
  //
  // For CBZ: ResetCbzTransientViewState(false) shuts the worker, frees cached
  // bitmaps, and leaves cbz_state and the archive entries intact.
  if (IsPdf() && mupdf_state && mupdf_state->worker) {
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] MuPDF worker shutdown begin book=%s",
             GetFileName() ? GetFileName() : "");
    ShutdownMuPdfWorker(mupdf_state);
    mupdf_state->worker_init_attempted = false;
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] MuPDF worker shutdown done book=%s",
             GetFileName() ? GetFileName() : "");
  }
  if (IsCbz()) {
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] CBZ worker shutdown begin book=%s",
             GetFileName() ? GetFileName() : "");
    ResetCbzTransientViewState(false);
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] CBZ worker shutdown done book=%s",
             GetFileName() ? GetFileName() : "");
  }
}

void Book::ResumeFixedLayoutWorkers() {
  // Restart background worker threads after HOME menu returns control.
  // Called from ReaderController::OnAppletResumed() on the main thread.
  if (IsPdf() && mupdf_state) {
    DBG_LOGF(GetStatusReporter(), "[APT][RESUME] MuPDF worker init begin book=%s",
             GetFileName() ? GetFileName() : "");
    InitMuPdfWorker(mupdf_state);
    DBG_LOGF(GetStatusReporter(), "[APT][RESUME] MuPDF worker init done worker=%s book=%s",
             mupdf_state->worker ? "ok" : "null (n/a)", GetFileName() ? GetFileName() : "");
  }
  if (IsCbz()) {
    DBG_LOGF(GetStatusReporter(), "[APT][RESUME] CBZ worker restart book=%s",
             GetFileName() ? GetFileName() : "");
    ResetCbzTransientViewState(true);
    DBG_LOGF(GetStatusReporter(), "[APT][RESUME] CBZ worker restart done book=%s",
             GetFileName() ? GetFileName() : "");
  }
}
