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

#include <expat.h>

#include <3ds.h>

#include "book/book.h"
#include "book/book_parser.h"
#include "book/book_renderer.h"
#include "formats/common/book_error.h"
#include "shared/app_flow_utils.h"
#include "shared/debug_runtime_mode.h"
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
#include "ui/text.h"

//! Book-related methods for App class.

namespace
{

  static const int kOpeningTitleMaxWidth = 216;
  static const int kOpeningTitleMaxLines = 3;
  static const int kOpeningTitleLineHeight = 16;

  // Returns a human-readable name for the book, for debug logging.
  [[gnu::unused]] static const char *SafeBookName(Book *book)
  {
    if (!book)
      return "(null)";
    if (book->GetFileName() && *book->GetFileName())
      return book->GetFileName();
    if (book->GetTitle() && *book->GetTitle())
      return book->GetTitle();
    return "(untitled)";
  }

  // Returns a copy of the bookmarks list with u16 values converted to int for easier handling in some contexts.
  std::list<int> CopyBookmarksAsInts(const std::list<u16> &bookmarks)
  {
    std::list<int> out;
    for (u16 bookmark : bookmarks)
      out.push_back((int)bookmark);
    return out;
  }

  // Applies a list of bookmarks (as ints) to the book's bookmarks list (as u16), replacing any existing bookmarks.
  void ApplyRemappedBookmarks(Book *book, const std::list<int> &bookmarks)
  {
    if (!book)
      return;
    std::list<u16> &dst = book->GetBookmarks();
    dst.clear();
    for (int bookmark : bookmarks)
      dst.push_back((u16)bookmark);
  }

  // State struct for tracking whether an open/reopen book operation requires a relayout and what the previous page/bookmark state was.
  struct OpenBookRelayoutState
  {
    bool needs_relayout;
    int old_page_count;
    int old_position;
    std::list<int> old_bookmarks;
  };

  std::string TrimAsciiWhitespaceLocal(const std::string &s)
  {
    size_t begin = 0;
    while (begin < s.size())
    {
      const unsigned char c = (unsigned char)s[begin];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        break;
      begin++;
    }
    size_t end = s.size();
    while (end > begin)
    {
      const unsigned char c = (unsigned char)s[end - 1];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        break;
      end--;
    }
    return s.substr(begin, end - begin);
  }

  std::string EllipsizeToWidth(Text *ts, const std::string &text, int max_width,
                               u8 style)
  {
    if (!ts)
      return text;
    if ((int)ts->GetStringWidth(text.c_str(), style) <= max_width)
      return text;

    const char *ellipsis = "...";
    if ((int)ts->GetStringWidth(ellipsis, style) > max_width)
      return std::string();

    for (size_t len = text.size(); len > 0; --len)
    {
      std::string candidate = text.substr(0, len);
      candidate += ellipsis;
      if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width)
        return candidate;
    }
    return std::string(ellipsis);
  }

  std::vector<std::string> BuildOpeningTitleLines(Text *ts, const char *name,
                                                  int max_width, int max_lines,
                                                  u8 style)
  {
    std::vector<std::string> lines;
    if (!ts || !name || !*name || max_lines <= 0)
      return lines;

    std::vector<std::string> tokens;
    std::string current_token;
    for (const char *p = name; *p; ++p)
    {
      const unsigned char c = (unsigned char)*p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      {
        if (!current_token.empty())
        {
          tokens.push_back(current_token);
          current_token.clear();
        }
      }
      else
      {
        current_token.push_back(*p);
      }
    }
    if (!current_token.empty())
      tokens.push_back(current_token);
    if (tokens.empty())
      return lines;

    std::string line;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      const std::string candidate =
          line.empty() ? tokens[i] : (line + " " + tokens[i]);
      if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width)
      {
        line = candidate;
        continue;
      }

      if (line.empty())
      {
        lines.push_back(EllipsizeToWidth(ts, tokens[i], max_width, style));
      }
      else
      {
        lines.push_back(line);
        line = tokens[i];
      }

      if ((int)lines.size() >= max_lines - 1 && i + 1 < tokens.size())
      {
        std::string rest = line;
        for (size_t j = i + 1; j < tokens.size(); ++j)
        {
          if (!rest.empty())
            rest.push_back(' ');
          rest += tokens[j];
        }
        lines.push_back(EllipsizeToWidth(ts, TrimAsciiWhitespaceLocal(rest),
                                         max_width, style));
        return lines;
      }
    }

    if (!line.empty() && (int)lines.size() < max_lines)
      lines.push_back(line);
    if ((int)lines.size() > max_lines)
      lines.resize((size_t)max_lines);
    return lines;
  }

  void ResetBookRenderState(App *app, bool clear_glyph_cache,
                            const char *reason)
  {
    if (!app || !app->ts.get())
      return;
    if (clear_glyph_cache)
      app->ts->ClearCache();
    app->ts->MarkAllScreensDirty();
    app->RequestStatusRedraw();
    if (reason)
      DBG_LOG(app, reason);
  }

  // Attempts to reuse the currently selected book by resetting transient state and redrawing the current page, returning whether the reuse was successful.
  bool ReuseParsedBook(App *app)
  {
    Book *selected = app ? app->GetSelectedBook() : NULL;
    if (!selected)
      return false;
    if (selected->IsCbz())
    {
      selected->ResetCbzFailureState();
      selected->ResetCbzTransientViewState(true);
      DBG_LOG(app, "BOOK reopen: state reset complete");
    }
    app->SetCurrentBook(selected);
    if (app->GetMode() == AppMode::Browser)
    {
      if (app->orientation)
      {
        // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
      }
      app->ShowCurrentBookView();
    }
    ResetBookRenderState(app, true,
                         "OpenBook: reset text renderer state (reuse)");
    Book *current = app->GetCurrentBook();
    if (current->GetPosition() >= current->GetPageCount())
      current->SetPosition(0);
    book_nav::DrawPage(current, app->ts.get());
    return true;
  }

  OpenBookRelayoutState CaptureRelayoutState(Book *book, bool needs_relayout)
  {
    OpenBookRelayoutState state = {needs_relayout, 0, 0, std::list<int>()};
    if (!book || !needs_relayout)
      return state;

    state.old_page_count = book->GetPageCount();
    state.old_position = book->GetPosition();
    state.old_bookmarks = CopyBookmarksAsInts(book->GetBookmarks());
    book->Close();
    return state;
  }

  static void DrawOpeningSplashImpl(App *app, unsigned spine_done,
                                    unsigned spine_total,
                                    const char *label = "opening book ...")
  {
    Book *selected = app ? app->GetSelectedBook() : NULL;
    if (!app || !app->ts || !selected)
      return;

    int savedStyle = app->ts->GetStyle();
    int savedColorMode = app->ts->GetColorMode();
    u16 *savedScreen = app->ts->GetScreen();

    app->ts->SetStyle(TEXT_STYLE_BROWSER);
    app->ts->PrintSplash(app->ts->screenleft);

    app->ts->SetScreen(app->ts->screenright);
    app->ts->ClearScreen();
    app->DrawBottomGradientBackground();
    app->ts->SetPen(12, 28);
    app->ts->PrintString(label ? label : "opening book ...");

    const char *name = selected->GetFileName();
    if (!name || !*name)
      name = selected->GetTitle();
    if (name && *name)
    {
      std::vector<std::string> lines = BuildOpeningTitleLines(
          app->ts.get(), name, kOpeningTitleMaxWidth, kOpeningTitleMaxLines,
          TEXT_STYLE_BROWSER);
      for (size_t i = 0; i < lines.size(); ++i)
      {
        app->ts->SetPen(12, (u16)(50 + (int)i * kOpeningTitleLineHeight));
        app->ts->PrintString(lines[i].c_str());
      }
    }

    if (spine_total > 0)
    {
      unsigned pct = spine_done * 100u / spine_total;
      char progress[32];
      snprintf(progress, sizeof(progress), "%u / %u  (%u%%)", spine_done,
               spine_total, pct);
      app->ts->SetPen(12, 106);
      app->ts->PrintString(progress);
    }

    app->ts->SetStyle(savedStyle);
    app->ts->SetColorMode(savedColorMode);
    app->ts->SetScreen(savedScreen);

    if (app->ShouldAbortWork())
      return;
    if (app->ts->BlitToFramebuffer())
    {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
  }

  void DrawOpeningSplash(App *app) { DrawOpeningSplashImpl(app, 0, 0); }

  static void MaybeDrawOpeningSplashProgress(App *app)
  {
    if (!app || !app->IsOpeningPending() || !app->GetOpeningBook())
      return;

    const unsigned int seq = app->GetOpeningProgressSeq();
    if (seq == 0 || seq == app->GetOpeningDrawnProgressSeq())
      return;

    DrawOpeningSplashImpl(app, app->GetOpeningSpineDone(),
                          app->GetOpeningSpineTotal());
    app->SetOpeningDrawnProgressSeq(seq);
  }

  void ResetOpeningState(App *app)
  {
    if (!app)
      return;
    app->SetOpeningPending(false);
    app->SetOpeningBook(NULL);
    app->SetOpeningSessionId(0);
    app->SetOpeningNeedsRelayout(false);
    app->SetOpeningOldPageCount(0);
    app->SetOpeningOldPosition(0);
    app->SetOpeningSpineProgress(0, 0);
    app->SetOpeningDrawnProgressSeq(0);
    app->MutableOpeningOldBookmarks().clear();
    app->SetOpeningStartedAtMs(0);
  }

  void DetachCurrentBookForSwitch(App *app, Book *next_book,
                                  unsigned int next_session_id,
                                  const char *reason)
  {
    if (!app)
      return;
    Book *current = app->GetCurrentBook();
    if (!ShouldCloseCurrentBookForSwitch(current, next_book))
      return;

    DBG_LOGF(app,
             "BOOK switch: close current session=%u current=%s next_session=%u next=%s reason=%s",
             app->GetCurrentBookSessionId(), SafeBookName(current),
             next_session_id, SafeBookName(next_book), reason ? reason : "");
    app->SetCurrentBook(NULL);
    app->SetCurrentBookSessionId(0);
    app->SetPdfDeferredReadyAtMs(0);
    current->Close();
    DBG_LOGF(app, "BOOK switch: current closed current=%s", SafeBookName(current));
  }

  u8 OpenSelectedBook(App *app, unsigned int session_id)
  {
    Book *selected = app ? app->GetSelectedBook() : NULL;
    if (!selected)
      return 254;

    DetachCurrentBookForSwitch(app, selected, session_id, "sync-open");
    selected->SetOpenSessionId(session_id);
    DBG_LOG(app, "BOOK open path: synchronous");
    DBG_LOGF(app, "BOOK open begin session=%u book=%s", session_id,
             SafeBookName(selected));
    if (int err = book_parser::Open(selected))
    {
      DBG_LOGF(app, "BOOK open fail session=%u rc=%d book=%s", session_id, err,
               SafeBookName(selected));
      if (const char *desc = DescribeBookOpenError(err))
      {
        app->PrintStatus(desc);
      }
      else
      {
        char msg[64];
        snprintf(msg, sizeof(msg), "error (%d)", err);
        app->PrintStatus(msg);
      }
      return (u8)err;
    }
    DBG_LOGF(app, "BOOK open success session=%u book=%s", session_id,
             SafeBookName(selected));
    return 0;
  }

  void CloseFailedOpenBook(App *app, Book *book, unsigned int session_id,
                           const char *reason)
  {
    if (!book)
      return;
    DBG_LOGF(app, "BOOK open cleanup: session=%u reason=%s pages=%d book=%s",
             session_id, reason ? reason : "", book->GetPageCount(),
             SafeBookName(book));
    book->Close();
  }

  void EnsureBookMode(App *app, const char *log_message)
  {
    if (!app || app->GetMode() != AppMode::Browser || app->ShouldAbortWork())
      return;
    if (app->orientation)
    {
      // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
    }
    app->ShowCurrentBookView();
    if (log_message)
      DBG_LOG(app, log_message);
  }

} // namespace

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
  opening_book->CancelAsyncReflowOpen();
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
    DrawOpeningSplashImpl(&app_, 0, 0, "saving cache...");
#ifdef DSLIBRIS_DEBUG
    u64 t0_flush = osGetTime();
#endif
    bookcurrent_->FlushPendingCacheSaves();
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(&app_, "FlushPendingCacheSaves (open): ms=%u", (unsigned)(osGetTime() - t0_flush));
#endif
  }

  app_.ShowCurrentBookView();
  DBG_LOG(&app_, "OpenBook: switched mode to APP_MODE_BOOK");

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

u8 ReaderController::OpenBook()
{
  Prefs *prefs = app_.prefs.get();
  Text *ts = app_.ts.get();
  Book *selected_book = app_.GetSelectedBook();
  Book *bookcurrent_ = app_.GetCurrentBook();

  if (app_.ShouldAbortWork())
    return BOOK_ERR_CANCELLED;

  //! Attempt to open book indicated by bookselected.

  if (!selected_book)
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

void App::parse_error(XML_Parser p)
{
  char msg[128];
  snprintf(msg, sizeof(msg), "%d:%d: %s\n",
           (int)XML_GetCurrentLineNumber(p),
           (int)XML_GetCurrentColumnNumber(p),
           XML_ErrorString(XML_GetErrorCode(p)));
  PrintStatus(msg);
}
