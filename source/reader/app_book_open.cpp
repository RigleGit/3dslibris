/*
    3dslibris - app_book_open.cpp
    Extracted from app_book.cpp. Holds ReaderController::OpenBook, the
    central open/reopen pipeline: fast-path reuse, async reflow path,
    synchronous parse path, deferred-relayout bookkeeping, and final
    mode switch to the reader view.

    Shared helpers (SafeBookName, ReuseParsedBook, CaptureRelayoutState,
    DrawOpeningSplash*, ResetOpeningState, DetachCurrentBookForSwitch,
    OpenSelectedBook, CloseFailedOpenBook, EnsureBookMode,
    ResetBookRenderState, ApplyRemappedBookmarks, and the
    OpenBookRelayoutState struct) live in app_book_internal.{h,cpp}.

    No behavior change — pure code motion.
*/

#include "app/app.h"
#include "app/reader_controller.h"

#include <stdio.h>

#include <3ds.h>

#include "book/book.h"
#include "book/book_renderer.h"
#include "formats/common/book_error.h"
#include "reader/app_book_internal.h"
#include "reader/book_page_nav.h"
#include "reader/deferred_relayout_utils.h"
#include "settings/prefs.h"
#include "shared/debug_log.h"
#include "ui/text.h"

using namespace reader_internal;

u8 ReaderController::OpenBook()
{
  Prefs *prefs = app_.prefs.get();
  Text *ts = app_.ts.get();
  Book *selected_book = app_.GetSelectedBook();
  Book *bookcurrent_ = app_.GetCurrentBook();

  if (app_.ShouldAbortWork())
    return BOOK_ERR_CANCELLED;

  //! Attempt to open book indicated by bookselected.

  if (!selected_book || selected_book->IsBrowserFolder())
    return 254;

  const bool needs_relayout = app_.BookNeedsRelayout(selected_book);
  const bool switching_books =
      (bookcurrent_ && bookcurrent_ != selected_book);
  const unsigned int session_id =
      switching_books ? app_.AllocateBookSessionId()
                      : (app_.GetCurrentBookSessionId()
                             ? app_.GetCurrentBookSessionId()
                             : app_.AllocateBookSessionId());
  DBG_LOGF(&app_,
           "BOOK switch: request session=%u current_session=%u current=%s selected=%s needs_relayout=%u async=%u",
           session_id, app_.GetCurrentBookSessionId(), SafeBookName(bookcurrent_),
           SafeBookName(selected_book), needs_relayout ? 1u : 0u,
           selected_book->SupportsAsyncReflowOpen() ? 1u : 0u);

  // Fast path: selected book is already parsed and resident.
  if (selected_book->GetPageCount() > 0 && !needs_relayout &&
      !selected_book->IsOpenAbortRequested())
  {
    ReuseParsedBook(&app_);
    app_.SetCurrentBookSessionId(session_id);
    if (app_.GetDeferredRelayoutBook() != app_.GetCurrentBook())
      ClearDeferredRelayoutState();
    app_.RequestStatusRedraw();
    app_.SetPrefsLayoutNoticePending(false);
    prefs->Write();
    return 0;
  }

  OpenBookRelayoutState relayout_state =
      CaptureRelayoutState(selected_book, needs_relayout);
  ClearDeferredRelayoutState();
  if (app_.IsOpeningPending() && app_.GetOpeningBook())
  {
    DBG_LOGF(&app_, "BOOK open reset: cancelling stale opening session=%u book=%s",
             app_.GetOpeningSessionId(), SafeBookName(app_.GetOpeningBook()));
    app_.GetOpeningBook()->CancelAsyncReflowOpen();
  }
  ResetOpeningState(&app_);

  // While parsing a new book, avoid displaying stale browser highlight state.
  DrawOpeningSplash(&app_);
  app_.SetOpeningSpineProgress(0, 0);
  app_.SetOpeningDrawnProgressSeq(0);
  if (app_.ShouldAbortWork())
  {
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return BOOK_ERR_CANCELLED;
  }

  if (selected_book->SupportsAsyncReflowOpen())
  {
    if (switching_books)
    {
      app_.PauseBrowserJobs();
    }
    DetachCurrentBookForSwitch(&app_, selected_book, session_id, "async-open");
    app_.SetOpeningPending(true);
    app_.SetOpeningBook(selected_book);
    app_.SetOpeningSessionId(session_id);
    app_.SetOpeningNeedsRelayout(relayout_state.needs_relayout);
    app_.SetOpeningOldPageCount(relayout_state.old_page_count);
    app_.SetOpeningOldPosition(relayout_state.old_position);
    app_.MutableOpeningOldBookmarks() = relayout_state.old_bookmarks;
    app_.SetOpeningStartedAtMs(osGetTime());
    app_.SetMode(AppMode::Opening);
    if (selected_book->StartAsyncReflowOpen(session_id))
    {
      DBG_LOGF(&app_, "REFLOW: async open submitted session=%u book=%s",
               session_id,
               selected_book->GetFileName()
                   ? selected_book->GetFileName()
                   : "");
      return 0;
    }
    ResetOpeningState(&app_);
    app_.SetMode(AppMode::Browser);
    DBG_LOGF(&app_, "REFLOW: async open fallback book=%s",
             selected_book->GetFileName()
                 ? selected_book->GetFileName()
                 : "");
  }
  if (u8 err = OpenSelectedBook(&app_, session_id))
  {
    CloseFailedOpenBook(&app_, selected_book, session_id,
                        err == BOOK_ERR_CANCELLED ? "cancelled"
                                                  : "sync-open-failed");
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return err;
  }
  if (app_.ShouldAbortWork() || selected_book->IsOpenAbortRequested())
  {
    CloseFailedOpenBook(&app_, selected_book, session_id, "post-open-abort");
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return BOOK_ERR_CANCELLED;
  }
  app_.SetCurrentBook(selected_book);
  app_.SetCurrentBookSessionId(session_id);
  bookcurrent_ = app_.GetCurrentBook();
  // Remember which layout generation produced these pages.
  bookcurrent_->SetLayoutRevision(app_.GetLayoutRevision());
  ResetBookRenderState(&app_, true,
                       "OpenBook: reset text renderer state (new open)");

  int pageCount = bookcurrent_->GetPageCount();
  DBG_LOGF(&app_, "Generated %d pages", pageCount);

  if (pageCount <= 0)
  {
    app_.PrintStatus("error: book has no parsed pages");
    CloseFailedOpenBook(&app_, bookcurrent_, session_id, "pages-zero");
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return 253;
  }

  // Keep the reader roughly in the same part of the book after repagination.
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

  EnsureBookMode(&app_, "OpenBook: switched mode to APP_MODE_BOOK");
  if (app_.ShouldAbortWork())
  {
    CloseFailedOpenBook(&app_, bookcurrent_, session_id, "post-mode-abort");
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return BOOK_ERR_CANCELLED;
  }

  if (bookcurrent_->HasPendingEpubPageCacheSave() ||
      bookcurrent_->HasPendingMobiPageCacheSave())
  {
    DrawOpeningSplashImpl(&app_, 0, 0, "saving cache...");
#ifdef DSLIBRIS_DEBUG
    u64 t0_flush2 = osGetTime();
#endif
    bookcurrent_->FlushPendingCacheSaves();
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(&app_, "FlushPendingCacheSaves (reopen): ms=%u", (unsigned)(osGetTime() - t0_flush2));
#endif
  }

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  book_nav::DrawPage(bookcurrent_, ts);
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    app_.SetPdfDeferredReadyAtMs(
        book_renderer::HasPendingFixedLayoutDeferredWork(bookcurrent_)
            ? (osGetTime() + book_renderer::GetFixedLayoutDeferredDelayMs(bookcurrent_))
            : 0);
  app_.RequestStatusRedraw();
  app_.SetPrefsLayoutNoticePending(false);
  prefs->Write();
  return 0;
}
