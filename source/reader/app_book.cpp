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
#include <sys/stat.h>

#include <expat.h>

#include <3ds.h>

#include "book/book.h"
#include "formats/common/book_error.h"
#include "shared/app_flow_utils.h"
#include "reader/deferred_relayout_utils.h"
#include "reader/book_switch_utils.h"
#include "reader/fixed_layout_input_utils.h"
#include "formats/common/pdf_view_utils.h"
#include "ui/button.h"
#include "debug_log.h"
#include "book/layout_reflow.h"
#include "book/page.h"
#include "parse.h"
#include "settings/prefs.h"
#include "ui/text.h"

//! Book-related methods for App class.

namespace {

static const int kPdfTouchRerenderDelta = 4;
static const u32 kMobiDeferredIdleDelayMs = 120;
static const u32 kMobiDeferredIdleBudgetMs = 12;
static const u16 kMobiDeferredIdlePageBudget = 8;
static const int kOpeningTitleMaxWidth = 216;
static const int kOpeningTitleMaxLines = 3;
static const int kOpeningTitleLineHeight = 16;

const char *SafeBookName(Book *book) {
  if (!book)
    return "(null)";
  if (book->GetFileName() && *book->GetFileName())
    return book->GetFileName();
  if (book->GetTitle() && *book->GetTitle())
    return book->GetTitle();
  return "(untitled)";
}

std::list<int> CopyBookmarksAsInts(const std::list<u16> &bookmarks) {
  std::list<int> out;
  for (u16 bookmark : bookmarks)
    out.push_back((int)bookmark);
  return out;
}

void ApplyRemappedBookmarks(Book *book, const std::list<int> &bookmarks) {
  if (!book)
    return;
  std::list<u16> *dst = book->GetBookmarks();
  dst->clear();
  for (int bookmark : bookmarks)
    dst->push_back((u16)bookmark);
}

struct OpenBookRelayoutState {
  bool needs_relayout;
  int old_page_count;
  int old_position;
  std::list<int> old_bookmarks;
};

std::string TrimAsciiWhitespaceLocal(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size()) {
    const unsigned char c = (unsigned char)s[begin];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    begin++;
  }
  size_t end = s.size();
  while (end > begin) {
    const unsigned char c = (unsigned char)s[end - 1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    end--;
  }
  return s.substr(begin, end - begin);
}

std::string EllipsizeToWidth(Text *ts, const std::string &text, int max_width,
                             u8 style) {
  if (!ts)
    return text;
  if ((int)ts->GetStringWidth(text.c_str(), style) <= max_width)
    return text;

  const char *ellipsis = "...";
  if ((int)ts->GetStringWidth(ellipsis, style) > max_width)
    return std::string();

  for (size_t len = text.size(); len > 0; --len) {
    std::string candidate = text.substr(0, len);
    candidate += ellipsis;
    if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width)
      return candidate;
  }
  return std::string(ellipsis);
}

std::vector<std::string> BuildOpeningTitleLines(Text *ts, const char *name,
                                                int max_width, int max_lines,
                                                u8 style) {
  std::vector<std::string> lines;
  if (!ts || !name || !*name || max_lines <= 0)
    return lines;

  std::vector<std::string> tokens;
  std::string current_token;
  for (const char *p = name; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!current_token.empty()) {
        tokens.push_back(current_token);
        current_token.clear();
      }
    } else {
      current_token.push_back(*p);
    }
  }
  if (!current_token.empty())
    tokens.push_back(current_token);
  if (tokens.empty())
    return lines;

  std::string line;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string candidate =
        line.empty() ? tokens[i] : (line + " " + tokens[i]);
    if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width) {
      line = candidate;
      continue;
    }

    if (line.empty()) {
      lines.push_back(EllipsizeToWidth(ts, tokens[i], max_width, style));
    } else {
      lines.push_back(line);
      line = tokens[i];
    }

    if ((int)lines.size() >= max_lines - 1 && i + 1 < tokens.size()) {
      std::string rest = line;
      for (size_t j = i + 1; j < tokens.size(); ++j) {
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

void DrawBookPage(Book *book, Text *ts) {
  if (!book || !ts || book->GetPageCount() == 0)
    return;
  book->DrawCurrentView(ts);
}

void ResetBookRenderState(App *app, bool clear_glyph_cache,
                          const char *reason) {
  if (!app || !app->ts)
    return;
  if (clear_glyph_cache)
    app->ts->ClearCache();
  app->ts->MarkAllScreensDirty();
  app->RequestStatusRedraw();
  if (reason)
    DBG_LOG(app, reason);
}

bool SetBookPage(Book *book, Text *ts, u16 page) {
  if (!book || !ts || page >= book->GetPageCount())
    return false;
  if (book->IsFixedLayout())
    book->CancelFixedLayoutDeferredWork();
  book->SetPosition(page);
  if (book->IsFixedLayout())
    book->ResetFixedLayoutViewportForNavigation();
  DrawBookPage(book, ts);
  return true;
}

bool TurnBookPage(Book *book, Text *ts, u16 *pagecurrent, u16 pagecount,
                  int delta) {
  if (!book || !ts || !pagecurrent || pagecount == 0)
    return false;
  if (delta < 0) {
    if (*pagecurrent == 0)
      return false;
  } else if (*pagecurrent >= pagecount - 1) {
    return false;
  }

  *pagecurrent = (u16)((int)*pagecurrent + delta);
  return SetBookPage(book, ts, *pagecurrent);
}

bool PumpDeferredMobi(Book *book, u32 budget_ms, u16 page_budget,
                      u16 *pagecount, bool *status_dirty,
                      bool *deferred_pumped) {
  if (!book || !pagecount || !status_dirty || !deferred_pumped ||
      !book->HasDeferredMobiParse()) {
    return false;
  }

  u16 before = book->GetPageCount();
  bool done = book->ContinueDeferredMobiParse(budget_ms, page_budget);
  u16 after = book->GetPageCount();
  if (after != before)
    *pagecount = after;
  if (after != before)
    *status_dirty = true;
  if (done)
    *status_dirty = true;
  *deferred_pumped = true;
  return (after != before) || done;
}

bool AdvanceBookPage(Book *book, Text *ts, u16 *pagecurrent, u16 *pagecount,
                     bool *status_dirty, bool *deferred_pumped) {
  if (!book || !ts || !pagecurrent || !pagecount || !status_dirty ||
      !deferred_pumped) {
    return false;
  }
  if (TurnBookPage(book, ts, pagecurrent, *pagecount, 1)) {
    *status_dirty = true;
    return true;
  }
  if (!book->HasDeferredMobiParse())
    return false;
  PumpDeferredMobi(book, 80, 18, pagecount, status_dirty, deferred_pumped);
  if (TurnBookPage(book, ts, pagecurrent, *pagecount, 1)) {
    *status_dirty = true;
    return true;
  }
  return false;
}

bool ReuseParsedBook(App *app) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!selected)
    return false;
  app->SetCurrentBook(selected);
  if (app->GetMode() == AppMode::Browser) {
    if (app->orientation) {
      // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
    }
    app->ShowCurrentBookView();
  }
  ResetBookRenderState(app, true,
                       "OpenBook: reset text renderer state (reuse)");
  Book *current = app->GetCurrentBook();
  if (current->GetPosition() >= current->GetPageCount())
    current->SetPosition(0);
  DrawBookPage(current, app->ts);
  return true;
}

OpenBookRelayoutState CaptureRelayoutState(Book *book, bool needs_relayout) {
  OpenBookRelayoutState state = {needs_relayout, 0, 0, std::list<int>()};
  if (!book || !needs_relayout)
    return state;

  state.old_page_count = book->GetPageCount();
  state.old_position = book->GetPosition();
  state.old_bookmarks = CopyBookmarksAsInts(*book->GetBookmarks());
  book->Close();
  return state;
}

void DrawOpeningSplash(App *app) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!app || !app->ts || !selected)
    return;

  int savedStyle = app->ts->GetStyle();
  int savedColorMode = app->ts->GetColorMode();
  u16 *savedScreen = app->ts->GetScreen();

  app->ts->SetStyle(TEXT_STYLE_BROWSER);
  app->ts->SetColorMode(0);
  app->ts->PrintSplash(app->ts->screenleft);

  app->ts->SetScreen(app->ts->screenright);
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();
  app->ts->SetPen(12, 28);
  app->ts->PrintString("opening book ...");

  const char *name = selected->GetFileName();
  if (!name || !*name)
    name = selected->GetTitle();
  if (name && *name) {
    std::vector<std::string> lines = BuildOpeningTitleLines(
        app->ts, name, kOpeningTitleMaxWidth, kOpeningTitleMaxLines,
        TEXT_STYLE_BROWSER);
    for (size_t i = 0; i < lines.size(); ++i) {
      app->ts->SetPen(12, (u16)(50 + (int)i * kOpeningTitleLineHeight));
      app->ts->PrintString(lines[i].c_str());
    }
  }

  app->ts->SetStyle(savedStyle);
  app->ts->SetColorMode(savedColorMode);
  app->ts->SetScreen(savedScreen);

  if (app->ts->BlitToFramebuffer()) {
    gfxFlushBuffers();
    gfxSwapBuffers();
  }
}

void ResetOpeningState(App *app) {
  if (!app)
    return;
  app->SetOpeningPending(false);
  app->SetOpeningBook(NULL);
  app->SetOpeningSessionId(0);
  app->SetOpeningNeedsRelayout(false);
  app->SetOpeningOldPageCount(0);
  app->SetOpeningOldPosition(0);
  app->MutableOpeningOldBookmarks().clear();
  app->SetOpeningStartedAtMs(0);
}

void DetachCurrentBookForSwitch(App *app, Book *next_book,
                                unsigned int next_session_id,
                                const char *reason) {
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
  app->SetMobiDeferredReadyAtMs(0);
  current->Close();
  DBG_LOGF(app, "BOOK switch: current closed current=%s", SafeBookName(current));
}

u8 OpenSelectedBook(App *app, unsigned int session_id) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!selected)
    return 254;

  DetachCurrentBookForSwitch(app, selected, session_id, "sync-open");
  DBG_LOGF(app, "BOOK open begin session=%u book=%s", session_id,
           SafeBookName(selected));
  if (int err = selected->Open()) {
    DBG_LOGF(app, "BOOK open fail session=%u rc=%d book=%s", session_id, err,
             SafeBookName(selected));
    if (const char *desc = DescribeBookOpenError(err)) {
      app->PrintStatus(desc);
    } else {
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

void EnsureBookMode(App *app, const char *log_message) {
  if (!app || app->GetMode() != AppMode::Browser)
    return;
  if (app->orientation) {
    // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
  }
  app->ShowCurrentBookView();
  if (log_message)
    DBG_LOG(app, log_message);
}

} // namespace

void ReaderController::ClearDeferredRelayoutState() {
  app_.SetDeferredRelayoutPending(false);
  app_.SetDeferredRelayoutBook(NULL);
  app_.SetDeferredRelayoutOldPageCount(0);
  app_.SetDeferredRelayoutOldPosition(0);
  app_.MutableDeferredRelayoutOldBookmarks().clear();
  app_.SetDeferredRelayoutInitialPosition(0);
}

void ReaderController::OnAppletSuspended() {
  Book *bookcurrent_ = app_.GetCurrentBook();
  Book *opening_book = app_.GetOpeningBook();
  app_.SetPdfTouchDragActive(false);
  app_.SetPdfTouchLastX(-1);
  app_.SetPdfTouchLastY(-1);
  app_.SetPdfDeferredReadyAtMs(0);
  app_.SetMobiDeferredReadyAtMs(0);
  if (bookcurrent_) {
    bookcurrent_->SetFixedLayoutViewportInteraction(false);
    bookcurrent_->CancelFixedLayoutDeferredWork();
  }
  if (!opening_book)
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_,
           "BOOK suspend: cancel opening session=%u book=%s async=%u mobi=%u",
           app_.GetOpeningSessionId(),
           opening_book->GetFileName() ? opening_book->GetFileName() : "",
           opening_book->IsAsyncReflowOpenPending() ? 1u : 0u,
           opening_book->HasDeferredMobiParse() ? 1u : 0u);
#endif
  opening_book->CancelFixedLayoutDeferredWork();
  opening_book->RequestAbortOpen();
  opening_book->CancelDeferredMobiParse();
  opening_book->CancelAsyncReflowOpen();
  app_.SetOpeningPending(false);
  app_.SetOpeningBook(NULL);
  app_.SetOpeningSessionId(0);
  app_.SetOpeningNeedsRelayout(false);
  app_.SetOpeningOldPageCount(0);
  app_.SetOpeningOldPosition(0);
  app_.MutableOpeningOldBookmarks().clear();
  app_.SetOpeningStartedAtMs(0);
  if (app_.GetMode() == AppMode::Opening) {
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
  }
}

void ReaderController::OnAppletResumed() {
  Book *bookcurrent_ = app_.GetCurrentBook();
  if (!bookcurrent_)
    return;
  if (bookcurrent_->IsFixedLayout()) {
    const u32 delay_ms = bookcurrent_->GetFixedLayoutDeferredDelayMs();
    app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
  }
  if (bookcurrent_->HasDeferredMobiParse())
    app_.SetMobiDeferredReadyAtMs(osGetTime() + kMobiDeferredIdleDelayMs);
}

bool ReaderController::MaybeFinalizeDeferredRelayout(Book *book, int page_count) {
  if (!book || !app_.IsDeferredRelayoutPending() ||
      app_.GetDeferredRelayoutBook() != book)
    return false;

  if (deferred_relayout_utils::ShouldCancelFinalDeferredRelayout(
          app_.IsDeferredRelayoutPending(), book->GetPosition(),
          app_.GetDeferredRelayoutInitialPosition())) {
    ClearDeferredRelayoutState();
    return false;
  }

  if (!deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
          app_.IsDeferredRelayoutPending(), book->HasDeferredMobiParse(),
          book->GetPosition(), app_.GetDeferredRelayoutInitialPosition())) {
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

void ReaderController::HandleEventInOpening() {
  Prefs *prefs = app_.prefs;
  Text *ts = app_.ts;
  const u32 keys = hidKeysDown();

  if (keys & (app_.key.b | app_.key.start | app_.key.select)) {
    Book *cancel_book = app_.GetOpeningBook();
    if (cancel_book) {
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(&app_, "BOOK open cancel: session=%u book=%s",
               app_.GetOpeningSessionId(),
               cancel_book->GetFileName() ? cancel_book->GetFileName() : "");
#endif
      cancel_book->RequestAbortOpen();
      cancel_book->CancelDeferredMobiParse();
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

  if (!app_.IsOpeningPending() || !app_.GetOpeningBook()) {
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
  const u64 elapsed_ms =
      app_.GetOpeningStartedAtMs() ? (osGetTime() - app_.GetOpeningStartedAtMs()) : 0;
  DBG_LOGF(&app_,
           "REFLOW: async open complete session=%u rc=%u ms=%llu book=%s",
           app_.GetOpeningSessionId(), (unsigned)err,
           (unsigned long long)elapsed_ms,
           opening_book->GetFileName() ? opening_book->GetFileName() : "");

  OpenBookRelayoutState relayout_state = {
      app_.IsOpeningNeedsRelayout(), app_.GetOpeningOldPageCount(),
      app_.GetOpeningOldPosition(), app_.MutableOpeningOldBookmarks()};

  ResetOpeningState(&app_);

  if (err) {
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetPdfDeferredReadyAtMs(0);
    app_.SetMobiDeferredReadyAtMs(0);
    ClearDeferredRelayoutState();
    if (err == BOOK_ERR_CANCELLED) {
      app_.SetMode(AppMode::Browser);
      app_.SetBrowserDirty(true);
      return;
    }
    if (const char *desc = DescribeBookOpenError(err)) {
      app_.PrintStatus(desc);
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "error (%d)", err);
      app_.PrintStatus(msg);
    }
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  if (opening_session_id == 0) {
    DBG_LOGF(&app_,
             "BOOK open stale-complete session=%u book=%s -> closing result",
             opening_session_id, SafeBookName(opening_book));
    opening_book->Close();
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

  int pageCount = bookcurrent_->GetPageCount();
  DBG_LOGF(&app_, "Generated %d pages", pageCount);

  if (pageCount <= 0) {
    app_.PrintStatus("error: book has no parsed pages");
    bookcurrent_->Close();
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return;
  }

  deferred_relayout_utils::OpenRelayoutPlan open_plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(
          relayout_state.needs_relayout, bookcurrent_->HasDeferredMobiParse(),
          relayout_state.old_page_count, relayout_state.old_position, pageCount,
          relayout_state.old_bookmarks);
  if (open_plan.has_remap) {
    bookcurrent_->SetPosition(open_plan.mapped_position);
    ApplyRemappedBookmarks(bookcurrent_, open_plan.mapped_bookmarks);
  }
  if (open_plan.defer_final_remap) {
    app_.SetDeferredRelayoutPending(true);
    app_.SetDeferredRelayoutBook(bookcurrent_);
    app_.SetDeferredRelayoutOldPageCount(relayout_state.old_page_count);
    app_.SetDeferredRelayoutOldPosition(relayout_state.old_position);
    app_.MutableDeferredRelayoutOldBookmarks() = relayout_state.old_bookmarks;
    app_.SetDeferredRelayoutInitialPosition(open_plan.mapped_position);
  } else {
    ClearDeferredRelayoutState();
  }
  app_.ShowCurrentBookView();
  DBG_LOG(&app_, "OpenBook: switched mode to APP_MODE_BOOK");

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  DrawBookPage(bookcurrent_, ts);
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    app_.SetPdfDeferredReadyAtMs(
        bookcurrent_->HasPendingFixedLayoutDeferredWork()
            ? (osGetTime() + bookcurrent_->GetFixedLayoutDeferredDelayMs())
            : 0);
  app_.SetMobiDeferredReadyAtMs(
      (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
          ? (osGetTime() + kMobiDeferredIdleDelayMs)
          : 0);
  app_.RequestStatusRedraw();
  app_.SetPrefsLayoutNoticePending(false);
  prefs->Write();
}

void ReaderController::HandleEventInBook() {
  Book *bookcurrent_ = app_.GetCurrentBook();
  Prefs *prefs = app_.prefs;
  Text *ts = app_.ts;
  decltype(App::key) &key = app_.key;
  auto touch_read = [&]() { return app_.TouchRead(); };
  auto request_status_redraw = [&]() { app_.RequestStatusRedraw(); };
  auto show_library_view = [&]() { app_.ShowLibraryView(); };
  auto show_settings_view = [&](bool from_book) { app_.ShowSettingsView(from_book); };

  u16 pagecurrent = bookcurrent_->GetPosition();
  u16 pagecount = bookcurrent_->GetPageCount();
  bool status_dirty = false;
  bool deferred_pumped = false;

  // Use 3DS edge-triggered key state to avoid carry-over/repeat from the key
  // press used to open the book.
  u32 keys = hidKeysDown();
  u32 held = hidKeysHeld();

  if (bookcurrent_ && bookcurrent_->IsFixedLayout()) {
    const auto delay_fixed_layout_deferred = [&]() {
      const u32 delay_ms = bookcurrent_->GetFixedLayoutDeferredDelayMs();
      app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
    };
    if (keys & KEY_A) {
      if (bookcurrent_->ChangeFixedLayoutZoom(1)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & KEY_B) {
      if (bookcurrent_->ChangeFixedLayoutZoom(-1)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (reader_input_utils::FixedLayoutSupportsShoulderPageTurn(
                   bookcurrent_->format) &&
               (keys & (key.r | KEY_R))) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, 1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (reader_input_utils::FixedLayoutSupportsShoulderPageTurn(
                   bookcurrent_->format) &&
               (keys & (key.l | KEY_L))) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & (key.right | KEY_RIGHT)) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, 1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & (key.left | KEY_LEFT)) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & (key.down | KEY_DOWN)) {
      if (!bookcurrent_->GetChapters().empty()) {
        if (bookcurrent_->JumpFixedLayoutChapter(1)) {
          bookcurrent_->ResetFixedLayoutViewportForNavigation();
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
          delay_fixed_layout_deferred();
        }
      } else if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, 1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & (key.up | KEY_UP)) {
      if (!bookcurrent_->GetChapters().empty()) {
        if (bookcurrent_->JumpFixedLayoutChapter(-1)) {
          bookcurrent_->ResetFixedLayoutViewportForNavigation();
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
          delay_fixed_layout_deferred();
        }
      } else if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & KEY_TOUCH) {
      touchPosition mapped = touch_read();
      app_.SetPdfTouchDragActive(true);
      bookcurrent_->SetFixedLayoutViewportInteraction(true);
      app_.SetPdfTouchLastX((int)mapped.px);
      app_.SetPdfTouchLastY((int)mapped.py);
      if (bookcurrent_->MoveFixedLayoutViewportToPreview((int)mapped.px,
                                                         (int)mapped.py)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (held & KEY_TOUCH) {
      touchPosition mapped = touch_read();
      if (!app_.IsPdfTouchDragActive() ||
          pdf_view_utils::TouchMovementExceedsThreshold(
              app_.GetPdfTouchLastX(), app_.GetPdfTouchLastY(), (int)mapped.px,
              (int)mapped.py, kPdfTouchRerenderDelta)) {
        app_.SetPdfTouchDragActive(true);
        bookcurrent_->SetFixedLayoutViewportInteraction(true);
        app_.SetPdfTouchLastX((int)mapped.px);
        app_.SetPdfTouchLastY((int)mapped.py);
        if (bookcurrent_->MoveFixedLayoutViewportToPreview((int)mapped.px,
                                                           (int)mapped.py)) {
          // DrawCurrentView now uses the full-page zoom cache for viewport
          // extraction (no MuPDF re-render), so updating both screens is fast.
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
          delay_fixed_layout_deferred();
        }
      }
    } else if (keys & KEY_START) {
      ts->SetStyle(TEXT_STYLE_BROWSER);
      ts->PrintSplash(ts->screenleft);
      show_library_view();
      prefs->Write();
    } else if (keys & KEY_SELECT) {
      show_settings_view(true);
      prefs->Write();
    }

    if (!(held & KEY_TOUCH)) {
      if (app_.IsPdfTouchDragActive()) {
        bookcurrent_->SetFixedLayoutViewportInteraction(false);
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
      bookcurrent_->SetFixedLayoutViewportInteraction(false);
      app_.SetPdfTouchDragActive(false);
      app_.SetPdfTouchLastX(-1);
      app_.SetPdfTouchLastY(-1);
    }

    if (status_dirty) {
      request_status_redraw();
    } else if (!(held & KEY_TOUCH) && keys == 0 &&
               bookcurrent_->HasPendingFixedLayoutDeferredWork() &&
               osGetTime() >= app_.GetPdfDeferredReadyAtMs()) {
      const u32 budget_ms = 4;
      const bool worked = bookcurrent_->PumpDeferredFixedLayoutWork(budget_ms);
      const u32 delay_ms = bookcurrent_->GetFixedLayoutDeferredDelayMs();
      app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
      if (worked) {
        DrawBookPage(bookcurrent_, ts);
        request_status_redraw();
      }
    }
    return;
  }

  if (keys & (KEY_A | key.r | key.down)) {
    // page forward.
    AdvanceBookPage(bookcurrent_, ts, &pagecurrent, &pagecount, &status_dirty,
                    &deferred_pumped);
  } else if (keys & (KEY_B | key.l | key.up)) {
    // page back.
    if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1))
      status_dirty = true;
  } else if (keys & KEY_X) {
    // cycle color modes: 0=normal, 1=dark, 2=sepia
    int mode = ts->GetColorMode();
    ts->SetColorMode((mode + 1) % 3);
    DrawBookPage(bookcurrent_, ts);
    status_dirty = true;
  } else if (keys & KEY_Y) {
    ToggleBookmark();
  } else if (keys & KEY_TOUCH) {
    // Page turn split follows visual horizontal axis (left/right) in both
    // orientations after central touch un-mirroring.
    touchPosition mapped = touch_read();
    const bool forward_zone = ((int)mapped.px >= 120);
    if (!forward_zone) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1))
        status_dirty = true;
    } else {
      AdvanceBookPage(bookcurrent_, ts, &pagecurrent, &pagecount, &status_dirty,
                      &deferred_pumped);
    }
  } else if (keys & KEY_START) {
    // Return to browser without reparsing the current book later.
    // Keep one parsed book resident in memory for fast reopen.
    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->PrintSplash(ts->screenleft);
    show_library_view();
    prefs->Write();
  } else if (keys & KEY_SELECT) {
    // Go directly to settings from book.
    show_settings_view(true);
    prefs->Write();
  } else if (keys & (key.right | key.left)) {
    // Navigate bookmarks.
    app_flow_utils::BookmarkJumpResult jump = app_flow_utils::FindBookmarkJumpTarget(
        *bookcurrent_->GetBookmarks(), bookcurrent_->GetPosition(),
        (keys & key.left) ? app_flow_utils::BookmarkJumpDirection::Next
                          : app_flow_utils::BookmarkJumpDirection::Previous);

    if (jump.found) {
      bookcurrent_->SetPosition(jump.page);
      DrawBookPage(bookcurrent_, ts);
      status_dirty = true;
    }
  }

  if (!deferred_pumped) {
    if (bookcurrent_->HasDeferredMobiParse()) {
      const u64 now = osGetTime();
      if (now >= app_.GetMobiDeferredReadyAtMs()) {
        PumpDeferredMobi(bookcurrent_, kMobiDeferredIdleBudgetMs,
                         kMobiDeferredIdlePageBudget, &pagecount,
                         &status_dirty, &deferred_pumped);
        app_.SetMobiDeferredReadyAtMs(
            bookcurrent_->HasDeferredMobiParse()
                ? (osGetTime() + kMobiDeferredIdleDelayMs)
                : 0);
      }
    } else {
      app_.SetMobiDeferredReadyAtMs(0);
    }
  }

  if (MaybeFinalizeDeferredRelayout(bookcurrent_, pagecount)) {
    DrawBookPage(bookcurrent_, ts);
    status_dirty = true;
  }

  if (status_dirty)
    request_status_redraw();
}

void ReaderController::ToggleBookmark() {
  Book *bookcurrent_ = app_.GetCurrentBook();
  Text *ts = app_.ts;

  if (!bookcurrent_ || !bookcurrent_->SupportsBookmarks())
    return;
  // Toggle bookmark for the current page.
  std::list<u16> *bookmarks = bookcurrent_->GetBookmarks();
  u16 pagecurrent = bookcurrent_->GetPosition();

  bool found = false;
  for (std::list<u16>::iterator i = bookmarks->begin(); i != bookmarks->end();
       i++) {
    if (*i == pagecurrent) {
      bookmarks->erase(i);
      found = true;
      break;
    }
  }

  if (!found) {
    bookmarks->push_back(pagecurrent);
    bookmarks->sort();
  }

  DrawBookPage(bookcurrent_, ts);
  const u32 delay_ms =
      bookcurrent_ ? bookcurrent_->GetFixedLayoutDeferredDelayMs() : 0;
  app_.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
  app_.RequestStatusRedraw();
}

void ReaderController::CloseBook() {
  Book *bookcurrent_ = app_.GetCurrentBook();

  if (bookcurrent_) {
    DBG_LOGF(&app_, "BOOK close current session=%u book=%s",
             app_.GetCurrentBookSessionId(), SafeBookName(bookcurrent_));
    bookcurrent_->Close();
    app_.SetCurrentBook(NULL);
    app_.SetCurrentBookSessionId(0);
  }
  app_.SetPdfDeferredReadyAtMs(0);
  app_.SetMobiDeferredReadyAtMs(0);
  ResetOpeningState(&app_);
  ClearDeferredRelayoutState();
}

int ReaderController::GetBookIndex(Book *b) {
  std::vector<Book *> &books = app_.books;

  if (!b)
    return -1;
  std::vector<Book *>::iterator it;
  int i = 0;
  for (it = books.begin(); it < books.end(); it++, i++) {
    if (*it == b)
      return i;
  }
  return -1;
}

u8 ReaderController::OpenBook() {
  Prefs *prefs = app_.prefs;
  Text *ts = app_.ts;
  Book *selected_book = app_.GetSelectedBook();
  Book *bookcurrent_ = app_.GetCurrentBook();

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
  if (selected_book->GetPageCount() > 0 && !needs_relayout) {
    ReuseParsedBook(&app_);
    app_.SetCurrentBookSessionId(session_id);
    if (app_.GetDeferredRelayoutBook() != app_.GetCurrentBook())
      ClearDeferredRelayoutState();
    bookcurrent_ = app_.GetCurrentBook();
    app_.SetMobiDeferredReadyAtMs(
        (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
            ? (osGetTime() + kMobiDeferredIdleDelayMs)
            : 0);
    app_.RequestStatusRedraw();
    app_.SetPrefsLayoutNoticePending(false);
    prefs->Write();
    return 0;
  }

  OpenBookRelayoutState relayout_state =
      CaptureRelayoutState(selected_book, needs_relayout);
  ClearDeferredRelayoutState();
  if (app_.IsOpeningPending() && app_.GetOpeningBook()) {
    DBG_LOGF(&app_, "BOOK open reset: cancelling stale opening session=%u book=%s",
             app_.GetOpeningSessionId(), SafeBookName(app_.GetOpeningBook()));
    app_.GetOpeningBook()->CancelAsyncReflowOpen();
  }
  ResetOpeningState(&app_);

  // While parsing a new book, avoid displaying stale browser highlight state.
  DrawOpeningSplash(&app_);

  if (selected_book->SupportsAsyncReflowOpen()) {
    DetachCurrentBookForSwitch(&app_, selected_book, session_id, "async-open");
    if (selected_book->StartAsyncReflowOpen(session_id)) {
      app_.SetOpeningPending(true);
      app_.SetOpeningBook(selected_book);
      app_.SetOpeningSessionId(session_id);
      app_.SetOpeningNeedsRelayout(relayout_state.needs_relayout);
      app_.SetOpeningOldPageCount(relayout_state.old_page_count);
      app_.SetOpeningOldPosition(relayout_state.old_position);
      app_.MutableOpeningOldBookmarks() = relayout_state.old_bookmarks;
      app_.SetOpeningStartedAtMs(osGetTime());
      DBG_LOGF(&app_, "REFLOW: async open submitted session=%u book=%s",
               session_id,
               selected_book->GetFileName()
                   ? selected_book->GetFileName()
                   : "");
      app_.SetMode(AppMode::Opening);
      return 0;
    }
    DBG_LOGF(&app_, "REFLOW: async open fallback book=%s",
             selected_book->GetFileName()
                 ? selected_book->GetFileName()
                 : "");
  }

  if (u8 err = OpenSelectedBook(&app_, session_id)) {
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return err;
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

  if (pageCount <= 0) {
    app_.PrintStatus("error: book has no parsed pages");
    bookcurrent_->Close();
    app_.SetCurrentBook(nullptr);
    app_.SetCurrentBookSessionId(0);
    app_.SetMode(AppMode::Browser);
    app_.SetBrowserDirty(true);
    return 253;
  }

  // Keep the reader roughly in the same part of the book after repagination.
  deferred_relayout_utils::OpenRelayoutPlan open_plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(
          relayout_state.needs_relayout, bookcurrent_->HasDeferredMobiParse(),
          relayout_state.old_page_count, relayout_state.old_position, pageCount,
          relayout_state.old_bookmarks);
  if (open_plan.has_remap) {
    bookcurrent_->SetPosition(open_plan.mapped_position);
    ApplyRemappedBookmarks(bookcurrent_, open_plan.mapped_bookmarks);
  }
  if (open_plan.defer_final_remap) {
    app_.SetDeferredRelayoutPending(true);
    app_.SetDeferredRelayoutBook(bookcurrent_);
    app_.SetDeferredRelayoutOldPageCount(relayout_state.old_page_count);
    app_.SetDeferredRelayoutOldPosition(relayout_state.old_position);
    app_.MutableDeferredRelayoutOldBookmarks() = relayout_state.old_bookmarks;
    app_.SetDeferredRelayoutInitialPosition(open_plan.mapped_position);
  } else {
    ClearDeferredRelayoutState();
  }

  EnsureBookMode(&app_, "OpenBook: switched mode to APP_MODE_BOOK");

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  DrawBookPage(bookcurrent_, ts);
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    app_.SetPdfDeferredReadyAtMs(
        bookcurrent_->HasPendingFixedLayoutDeferredWork()
            ? (osGetTime() + bookcurrent_->GetFixedLayoutDeferredDelayMs())
            : 0);
  app_.SetMobiDeferredReadyAtMs(
      (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
          ? (osGetTime() + kMobiDeferredIdleDelayMs)
          : 0);
  app_.RequestStatusRedraw();
  app_.SetPrefsLayoutNoticePending(false);
  prefs->Write();
  return 0;
}

void App::parse_error(XML_Parser p) {
  char msg[128];
  snprintf(msg, sizeof(msg), "%d:%d: %s\n",
           (int)XML_GetCurrentLineNumber(p),
           (int)XML_GetCurrentColumnNumber(p),
           XML_ErrorString(XML_GetErrorCode(p)));
  PrintStatus(msg);
}
