/*
    3dslibris - app_book.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Handles open/reopen flow for selected books.
    - Runs parse pipeline by format and switches app mode to reading view.
    - Applies 3DS input mapping and draw/status refresh synchronization.
*/

#include "app/app.h"
#include "app/reader_controller.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <expat.h>

#include <3ds.h>

#include "book/book.h"
#include "book/book_parser.h"
#include "book/book_renderer.h"
#include "formats/common/book_error.h"
#include "shared/app_flow_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/string_utils.h"
#include "reader/book_page_nav.h"
#include "reader/book_switch_utils.h"
#include "reader/deferred_relayout_utils.h"
#include "reader/fixed_layout_reader_input.h"
#include "reader/reader_controls.h"
#include "reader/reflow_reader_input.h"
#include "reader/suspend_policy_utils.h"
#include "ui/button.h"
#include "shared/debug_log.h"
#include "book/layout_reflow.h"
#include "parse.h"
#include "settings/prefs.h"
#include "shared/boot_trace.h"
#include "ui/text.h"

#include "reader/app_book_internal.h"

//! Book-related methods for App class.

// Pull the shared helpers (moved to reader_internal in app_book_internal.cpp)
// into this TU's lookup so existing unqualified call sites still resolve.
using namespace reader_internal;

void DrawOpeningSplashWithProgress(unsigned done, unsigned total,
                                   void *user_data)
{
  App *app = static_cast<App *>(user_data);
  if (!app)
    return;
  if (!app->IsOpeningPending())
  {
    DrawOpeningSplashImpl(app, done, total);
    return;
  }
  app->SetOpeningSpineProgress(done, total);
}

void ReaderController::ClearDeferredRelayoutState()
{
  app_.SetDeferredRelayoutPending(false);
  app_.SetDeferredRelayoutBook(NULL);
  app_.SetDeferredRelayoutOldPageCount(0);
  app_.SetDeferredRelayoutOldPosition(0);
  app_.MutableDeferredRelayoutOldBookmarks().clear();
  app_.SetDeferredRelayoutInitialPosition(0);
}

void ReaderController::OnAppletSuspendRequested()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  Book *opening_book = app_.GetOpeningBook();
  app_.SetPdfTouchDragActive(false);
  app_.SetPdfTouchLastX(-1);
  app_.SetPdfTouchLastY(-1);
  app_.SetPdfDeferredReadyAtMs(0);
  if (bookcurrent_)
    book_renderer::SetFixedLayoutViewportInteraction(bookcurrent_, false);
  if (opening_book)
  {
    opening_book->RequestAbortOpen();
  }
}

void ReaderController::OnAppletSuspended()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  Book *opening_book = app_.GetOpeningBook();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_,
           "[APT][SUSPEND] entry mode=%d has_current=%d has_opening=%d",
           (int)app_.GetMode(),
           bookcurrent_ ? 1 : 0,
           opening_book ? 1 : 0);
#endif
  app_.SetPdfTouchDragActive(false);
  app_.SetPdfTouchLastX(-1);
  app_.SetPdfTouchLastY(-1);
  app_.SetPdfDeferredReadyAtMs(0);
  if (bookcurrent_)
  {
    book_renderer::SetFixedLayoutViewportInteraction(bookcurrent_, false);
    book_renderer::CancelFixedLayoutDeferredWork(bookcurrent_);
    bookcurrent_->SuspendFixedLayoutWorkers();
    // Reflow worker (EPUB/MOBI/FB2/...) on core 1 — same HOME-panic risk as
    // MuPDF/CBZ workers when left alive across suspend. Signal-only; the
    // join completes on resume.
    bookcurrent_->SignalReflowWorkerShutdown();
  }
  if (!opening_book)
    return;
  book_renderer::CancelFixedLayoutDeferredWork(opening_book);
  if (reader_suspend_policy_utils::ShouldKeepOpeningDuringSuspend(
          true, opening_book->IsAsyncReflowOpenPending()))
  {
    // The OS suspends all threads (including the core-1 worker) while the
    // HOME menu is active. Leave the opening in progress so it completes
    // transparently on resume — no blocking join, no state teardown.
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(&app_,
             "BOOK suspend: keeping async open alive session=%u book=%s",
             app_.GetOpeningSessionId(),
             opening_book->GetFileName() ? opening_book->GetFileName() : "");
#endif
    return;
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_,
           "BOOK suspend: cancel opening session=%u book=%s",
           app_.GetOpeningSessionId(),
           opening_book->GetFileName() ? opening_book->GetFileName() : "");
#endif
  opening_book->RequestAbortOpen();
  // Suspend path: use signal-only shutdown. CancelAsyncReflowOpen blocks
  // joining the worker (100ms loops, potentially seconds mid-parse), which
  // delays HandleAppletSuspend past the HOME menu's acknowledgment window.
  opening_book->SignalReflowWorkerShutdown();
  app_.SetOpeningPending(false);
  app_.SetOpeningBook(NULL);
  app_.SetOpeningSessionId(0);
  app_.SetOpeningNeedsRelayout(false);
  app_.SetOpeningOldPageCount(0);
  app_.SetOpeningOldPosition(0);
  app_.MutableOpeningOldBookmarks().clear();
  app_.SetOpeningStartedAtMs(0);
  if (app_.GetMode() == AppMode::Opening)
  {
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
  }
}

void ReaderController::OnAppletResumed()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  if (!bookcurrent_)
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_,
           "[APT][RESUME] entry mode=%d is_fixed=%d book=%s",
           (int)app_.GetMode(),
           bookcurrent_->IsFixedLayout() ? 1 : 0,
           bookcurrent_->GetFileName() ? bookcurrent_->GetFileName() : "");
#endif
  // Complete the deferred reflow-worker join from suspend. Non-blocking —
  // if the worker hasn't exited yet, the next StartAsyncReflowOpen call
  // picks it up via its existing non-blocking cleanup branch.
  bookcurrent_->FinishShutdownReflowWorker();
  if (bookcurrent_->IsFixedLayout())
  {
    bookcurrent_->ResumeFixedLayoutWorkers();
    const u32 delay_ms = book_renderer::GetFixedLayoutDeferredDelayMs(bookcurrent_);
    app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
  }
}

bool ReaderController::MaybeFinalizeDeferredRelayout(Book *book, int page_count)
{
  if (!book || !app_.IsDeferredRelayoutPending() ||
      app_.GetDeferredRelayoutBook() != book)
    return false;

  if (deferred_relayout_utils::ShouldCancelFinalDeferredRelayout(
          app_.IsDeferredRelayoutPending(), book->GetPosition(),
          app_.GetDeferredRelayoutInitialPosition()))
  {
    ClearDeferredRelayoutState();
    return false;
  }

  if (!deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
          app_.IsDeferredRelayoutPending(), false,
          book->GetPosition(), app_.GetDeferredRelayoutInitialPosition()))
  {
    return false;
  }

  book->SetPosition(layout_reflow::RemapPageIndexApprox(
      app_.GetDeferredRelayoutOldPosition(),
      app_.GetDeferredRelayoutOldPageCount(),
      page_count));
  ApplyRemappedBookmarks(
      book, layout_reflow::RemapBookmarksApprox(
                app_.MutableDeferredRelayoutOldBookmarks(),
                app_.GetDeferredRelayoutOldPageCount(), page_count));
  ClearDeferredRelayoutState();
  return true;
}

void ReaderController::HandleEventInOpening()
{
  Prefs *prefs = app_.prefs.get();
  Text *ts = app_.ts.get();
  if (app_.ShouldAbortWork())
    return;
  MaybeDrawOpeningSplashProgress(&app_);
  const u32 keys = hidKeysDown();

  if (keys & (app_.key.b | app_.key.start | app_.key.select))
  {
    Book *cancel_book = app_.GetOpeningBook();
    if (cancel_book)
    {
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(&app_, "BOOK open cancel: session=%u book=%s",
               app_.GetOpeningSessionId(),
               cancel_book->GetFileName() ? cancel_book->GetFileName() : "");
#endif
      cancel_book->RequestAbortOpen();
      cancel_book->CancelAsyncReflowOpen();
    }
    app_.SetOpeningPending(false);
    app_.SetOpeningBook(NULL);
    app_.SetOpeningSessionId(0);
    app_.SetOpeningNeedsRelayout(false);
    app_.SetOpeningOldPageCount(0);
    app_.SetOpeningOldPosition(0);
    app_.MutableOpeningOldBookmarks().clear();
    app_.SetOpeningStartedAtMs(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  if (!app_.IsOpeningPending() || !app_.GetOpeningBook())
  {
    ResetOpeningState(&app_);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  Book *opening_book = app_.GetOpeningBook();
  const unsigned int opening_session_id = app_.GetOpeningSessionId();
  if (!opening_book->PumpAsyncReflowOpen())
    return;

  boot_trace::Boot("async open complete begin");
  const u8 err = opening_book->ConsumeAsyncReflowOpenResult();
#ifdef DSLIBRIS_DEBUG
  {
    const u64 elapsed_ms =
        app_.GetOpeningStartedAtMs() ? (osGetTime() - app_.GetOpeningStartedAtMs()) : 0;
    DBG_LOGF(&app_,
             "REFLOW: async open complete session=%u rc=%u ms=%llu book=%s",
             app_.GetOpeningSessionId(), (unsigned)err,
             (unsigned long long)elapsed_ms,
             opening_book->GetFileName() ? opening_book->GetFileName() : "");
  }
#endif

  OpenBookRelayoutState relayout_state = {
      app_.IsOpeningNeedsRelayout(), app_.GetOpeningOldPageCount(),
      app_.GetOpeningOldPosition(), app_.MutableOpeningOldBookmarks()};

  ResetOpeningState(&app_);

  if (err)
  {
    boot_trace::Boot("async open failed");
    CloseFailedOpenBook(&app_, opening_book, opening_session_id,
                        err == BOOK_ERR_CANCELLED ? "cancelled"
                                                  : "async-open-failed");
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetPdfDeferredReadyAtMs(0);
    ClearDeferredRelayoutState();
    if (err == BOOK_ERR_CANCELLED)
    {
      app_.SetMode(AppMode::Browser);
      app_.SetBrowserDirty(true);
      return;
    }
    if (const char *desc = DescribeBookOpenError(err))
    {
      app_.PrintStatus(desc);
    }
    else
    {
      char msg[64];
      snprintf(msg, sizeof(msg), "error (%d)", err);
      app_.PrintStatus(msg);
    }
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  const int pageCount = opening_book->GetPageCount();
  if (!ShouldAttachOpeningResult(opening_session_id,
                                 opening_book->GetOpenSessionId(),
                                 opening_book->IsOpenAbortRequested(),
                                 pageCount))
  {
    boot_trace::Boot("async open attach denied");
    const char *cause = DescribeOpeningFailureCause(
        opening_session_id, opening_book->GetOpenSessionId(),
        opening_book->IsOpenAbortRequested(), pageCount);
    DBG_LOGF(&app_,
             "BOOK attach denied: cause=%s session=%u book_session=%u pages=%d book=%s",
             cause, opening_session_id, opening_book->GetOpenSessionId(),
             pageCount, SafeBookName(opening_book));
    if (strcmp(cause, "aborted") != 0)
      app_.PrintStatus("error: book has no parsed pages");
    CloseFailedOpenBook(&app_, opening_book, opening_session_id, cause);
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  app_.SetCurrentBook(opening_book);
  app_.SetCurrentBookSessionId(opening_session_id);
  Book *bookcurrent_ = app_.GetCurrentBook();
  bookcurrent_->SetLayoutRevision(app_.GetLayoutRevision());
  DBG_LOGF(&app_, "BOOK open attach session=%u book=%s", opening_session_id,
           SafeBookName(bookcurrent_));
  DBG_LOGF(&app_, "Generated %d pages", pageCount);

  deferred_relayout_utils::OpenRelayoutPlan open_plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(
          relayout_state.needs_relayout, false,
          relayout_state.old_page_count, relayout_state.old_position, pageCount,
          relayout_state.old_bookmarks);
  if (open_plan.has_remap)
  {
    bookcurrent_->SetPosition(open_plan.mapped_position);
    ApplyRemappedBookmarks(bookcurrent_, open_plan.mapped_bookmarks);
  }
  if (open_plan.defer_final_remap)
  {
    app_.SetDeferredRelayoutPending(true);
    app_.SetDeferredRelayoutBook(bookcurrent_);
    app_.SetDeferredRelayoutOldPageCount(relayout_state.old_page_count);
    app_.SetDeferredRelayoutOldPosition(relayout_state.old_position);
    app_.MutableDeferredRelayoutOldBookmarks() = relayout_state.old_bookmarks;
    app_.SetDeferredRelayoutInitialPosition(open_plan.mapped_position);
  }
  else
  {
    ClearDeferredRelayoutState();
  }
  if (bookcurrent_->HasPendingEpubPageCacheSave() ||
      bookcurrent_->HasPendingMobiPageCacheSave())
  {
    boot_trace::Boot("async open cache save begin");
    DrawOpeningSplashImpl(&app_, 0, 0, "saving cache...");
#ifdef DSLIBRIS_DEBUG
    u64 t0_flush = osGetTime();
#endif
    bookcurrent_->FlushPendingCacheSaves();
    boot_trace::Boot("async open cache save done");
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(&app_, "FlushPendingCacheSaves (open): ms=%u", (unsigned)(osGetTime() - t0_flush));
#endif
  }

  app_.ShowCurrentBookView();
  boot_trace::Boot("async open show book view");
  DBG_LOG(&app_, "OpenBook: switched mode to APP_MODE_BOOK");

  bookcurrent_->SetLastOpenedTime((uint32_t)time(NULL));
  if (app_.prefs)
    app_.prefs->Write();

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  book_nav::DrawPage(bookcurrent_, ts);
  boot_trace::Boot("async open first page drawn");
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    app_.SetPdfDeferredReadyAtMs(
        book_renderer::HasPendingFixedLayoutDeferredWork(bookcurrent_)
            ? (osGetTime() + book_renderer::GetFixedLayoutDeferredDelayMs(bookcurrent_))
            : 0);
  app_.RequestStatusRedraw();
  app_.SetPrefsLayoutNoticePending(false);
  prefs->Write();
  boot_trace::Boot("async open done");
}

void ReaderController::HandleEventInBook()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  Text *ts = app_.ts.get();
  if (app_.ShouldAbortWork() || !bookcurrent_)
    return;

  const ReaderControls ctrl = BuildPortraitControls(app_.key);
  const u32 keys = hidKeysDown();
  const u32 held = hidKeysHeld();
  u16 pagecurrent = bookcurrent_->GetPosition();
  u16 pagecount = bookcurrent_->GetPageCount();

  if (bookcurrent_->IsFixedLayout()) {
    if (fixed_layout_input::HandleInBook(app_, bookcurrent_, ts, keys, held,
                                         &pagecurrent, pagecount, ctrl))
      app_.RequestStatusRedraw();
    return;
  }

  bool status_dirty = reflow_input::HandleInBook(
      app_, bookcurrent_, ts, app_.prefs.get(), keys, held,
      &pagecurrent, &pagecount, ctrl);
  if (MaybeFinalizeDeferredRelayout(bookcurrent_, (int)pagecount)) {
    book_nav::DrawPage(bookcurrent_, ts);
    status_dirty = true;
  }
  if (status_dirty)
    app_.RequestStatusRedraw();
}

void ReaderController::ToggleBookmark()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  Text *ts = app_.ts.get();

  if (!bookcurrent_ || !bookcurrent_->SupportsBookmarks())
    return;
  // Toggle bookmark for the current page.
  std::list<u16> &bookmarks = bookcurrent_->GetBookmarks();
  u16 pagecurrent = bookcurrent_->GetPosition();

  bool found = false;
  for (std::list<u16>::iterator i = bookmarks.begin(); i != bookmarks.end();
       i++)
  {
    if (*i == pagecurrent)
    {
      bookmarks.erase(i);
      found = true;
      break;
    }
  }

  if (!found)
  {
    auto it = std::lower_bound(bookmarks.begin(), bookmarks.end(), pagecurrent);
    bookmarks.insert(it, pagecurrent);
  }

  book_nav::DrawPage(bookcurrent_, ts);
  const u32 delay_ms =
      bookcurrent_ ? book_renderer::GetFixedLayoutDeferredDelayMs(bookcurrent_) : 0;
  app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
  app_.RequestStatusRedraw();
}

void ReaderController::CloseBook()
{
  Book *bookcurrent_ = app_.GetCurrentBook();
  app_.SetInlineLinkFocusActive(false);
  app_.SetInlineLinkHoldArmed(false);
  app_.SetInlineLinkHoldConsumed(false);
  app_.SetInlineLinkHoldStartedAtMs(0);

  if (bookcurrent_)
  {
    DBG_LOGF(&app_, "BOOK close current session=%u book=%s",
             app_.GetCurrentBookSessionId(), SafeBookName(bookcurrent_));
    bookcurrent_->Close();
    app_.SetCurrentBook(NULL);
    app_.SetCurrentBookSessionId(0);
  }
  app_.SetPdfDeferredReadyAtMs(0);
  ResetOpeningState(&app_);
  ClearDeferredRelayoutState();
}

int ReaderController::GetBookIndex(Book *b)
{
  std::vector<Book *> &books = app_.books;

  if (!b)
    return -1;
  std::vector<Book *>::iterator it;
  int i = 0;
  for (it = books.begin(); it < books.end(); it++, i++)
  {
    if (*it == b)
      return i;
  }
  return -1;
}

// NOTE: ReaderController::OpenBook moved to app_book_open.cpp.

void App::parse_error(XML_Parser p)
{
  char msg[128];
  snprintf(msg, sizeof(msg), "%d:%d: %s\n",
           (int)XML_GetCurrentLineNumber(p),
           (int)XML_GetCurrentColumnNumber(p),
           XML_ErrorString(XML_GetErrorCode(p)));
  PrintStatus(msg);
}
