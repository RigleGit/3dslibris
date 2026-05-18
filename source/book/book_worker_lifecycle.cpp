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
  // For MuPDF: signal shutdown but do NOT join. ShutdownMuPdfWorker joins the
  // strip-renderer thread, which can take 2-5s mid-decode. Blocking here delays
  // aptMainLoop() HOME acknowledgment past the HOME menu's tolerance window and
  // triggers a HOME panic. The OS suspends the worker during HOME; the join
  // completes in milliseconds on resume (thread sees shutdown_requested and exits).
  // For CBZ: ShutdownCbzWorker joins explicitly here because stb_image decodes
  // complete in <500ms and the join is safe within the acknowledgment window.
  if (IsPdf() && mupdf_state && mupdf_state->worker) {
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] MuPDF worker signal shutdown book=%s",
             GetFileName() ? GetFileName() : "");
    SignalMuPdfWorkerShutdown(mupdf_state);
    mupdf_state->worker_init_attempted = false;
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] MuPDF worker signaled book=%s",
             GetFileName() ? GetFileName() : "");
  }
  if (IsCbz() && cbz_state) {
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] CBZ worker shutdown begin book=%s",
             GetFileName() ? GetFileName() : "");
    ShutdownCbzWorker(cbz_state);
    ResetCbzTransientViewState(false);
    DBG_LOGF(GetStatusReporter(), "[APT][SUSPEND] CBZ worker shutdown done book=%s",
             GetFileName() ? GetFileName() : "");
  }
}

void Book::ResumeFixedLayoutWorkers() {
  // Restart background worker threads after HOME menu returns control.
  // Called from ReaderController::OnAppletResumed() on the main thread.
  if (IsPdf() && mupdf_state) {
    if (mupdf_state->worker) {
      // Complete the deferred join from SuspendFixedLayoutWorkers. The worker
      // was OS-suspended during HOME and was signaled to shut down before that,
      // so it exits almost immediately — at most finishing the current decode
      // strip, then seeing shutdown_requested at the top of its loop.
      DBG_LOGF(GetStatusReporter(), "[APT][RESUME] MuPDF worker join begin book=%s",
               GetFileName() ? GetFileName() : "");
      ShutdownMuPdfWorker(mupdf_state);
      DBG_LOGF(GetStatusReporter(), "[APT][RESUME] MuPDF worker join done book=%s",
               GetFileName() ? GetFileName() : "");
    }
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
