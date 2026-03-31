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
#include "shared/deferred_relayout_utils.h"
#include "shared/pdf_view_utils.h"
#include "ui/button.h"
#include "debug_log.h"
#include "book/layout_reflow.h"
#include "main.h"
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
    DBG_LOG(app, "OpenBook: reused parsed book");
  }
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

u8 OpenSelectedBook(App *app) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!selected)
    return 254;

  if (app->GetCurrentBook() && app->GetCurrentBook() != selected)
    app->GetCurrentBook()->Close();
  if (int err = selected->Open()) {
    if (const char *desc = DescribeBookOpenError(err)) {
      app->PrintStatus(desc);
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "error (%d)", err);
      app->PrintStatus(msg);
    }
    return (u8)err;
  }
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

void App::ClearDeferredRelayoutState() {
  deferred_relayout_.pending = false;
  deferred_relayout_.book = NULL;
  deferred_relayout_.old_page_count = 0;
  deferred_relayout_.old_position = 0;
  deferred_relayout_.old_bookmarks.clear();
  deferred_relayout_.initial_position = 0;
}

bool App::MaybeFinalizeDeferredRelayout(Book *book, int page_count) {
  if (!book || !deferred_relayout_.pending || deferred_relayout_.book != book)
    return false;

  if (deferred_relayout_utils::ShouldCancelFinalDeferredRelayout(
          deferred_relayout_.pending, book->GetPosition(),
          deferred_relayout_.initial_position)) {
    ClearDeferredRelayoutState();
    return false;
  }

  if (!deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
          deferred_relayout_.pending, book->HasDeferredMobiParse(),
          book->GetPosition(), deferred_relayout_.initial_position)) {
    return false;
  }

  book->SetPosition(layout_reflow::RemapPageIndexApprox(
      deferred_relayout_.old_position, deferred_relayout_.old_page_count,
      page_count));
  ApplyRemappedBookmarks(
      book, layout_reflow::RemapBookmarksApprox(
                deferred_relayout_.old_bookmarks,
                deferred_relayout_.old_page_count, page_count));
  ClearDeferredRelayoutState();
  return true;
}

void App::HandleEventInOpening() {
  if (!opening_.pending || !opening_.book) {
    mode_ = AppMode::Browser;
    browser_.view_dirty = true;
    return;
  }

  Book *opening_book = opening_.book;
  if (!opening_book->PumpAsyncReflowOpen())
    return;

  const u8 err = opening_book->ConsumeAsyncReflowOpenResult();
  const u64 elapsed_ms =
      opening_.started_at_ms ? (osGetTime() - opening_.started_at_ms) : 0;
  DBG_LOGF(this, "REFLOW: async open complete rc=%u ms=%llu book=%s",
           (unsigned)err, (unsigned long long)elapsed_ms,
           opening_book->GetFileName() ? opening_book->GetFileName() : "");

  OpenBookRelayoutState relayout_state = {
      opening_.needs_relayout, opening_.old_page_count, opening_.old_position,
      opening_.old_bookmarks};

  opening_.pending = false;
  opening_.book = NULL;
  opening_.needs_relayout = false;
  opening_.old_page_count = 0;
  opening_.old_position = 0;
  opening_.old_bookmarks.clear();
  opening_.started_at_ms = 0;

  if (err) {
    ClearDeferredRelayoutState();
    if (const char *desc = DescribeBookOpenError(err)) {
      PrintStatus(desc);
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "error (%d)", err);
      PrintStatus(msg);
    }
    mode_ = AppMode::Browser;
    browser_.view_dirty = true;
    return;
  }

  bookcurrent_ = opening_book;
  bookcurrent_->SetLayoutRevision(layout_revision);

  int pageCount = bookcurrent_->GetPageCount();
  DBG_LOGF(this, "Generated %d pages", pageCount);

  if (pageCount <= 0) {
    PrintStatus("error: book has no parsed pages");
    bookcurrent_->Close();
    bookcurrent_ = nullptr;
    mode_ = AppMode::Browser;
    browser_.view_dirty = true;
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
    deferred_relayout_.pending = true;
    deferred_relayout_.book = bookcurrent_;
    deferred_relayout_.old_page_count = relayout_state.old_page_count;
    deferred_relayout_.old_position = relayout_state.old_position;
    deferred_relayout_.old_bookmarks = relayout_state.old_bookmarks;
    deferred_relayout_.initial_position = open_plan.mapped_position;
  } else {
    ClearDeferredRelayoutState();
  }
  ShowCurrentBookView();
  DBG_LOG(this, "OpenBook: switched mode to APP_MODE_BOOK");

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  DrawBookPage(bookcurrent_, ts);
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    pdf_deferred_ready_at_ms_ =
        bookcurrent_->HasPendingFixedLayoutDeferredWork()
            ? (osGetTime() + bookcurrent_->GetFixedLayoutDeferredDelayMs())
            : 0;
  mobi_deferred_ready_at_ms_ =
      (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
          ? (osGetTime() + kMobiDeferredIdleDelayMs)
          : 0;
  RequestStatusRedraw();
  prefs_view_.layout_notice_pending = false;
  prefs->Write();
}

void App::HandleEventInBook() {
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
      pdf_deferred_ready_at_ms_ = delay_ms ? (osGetTime() + delay_ms) : 0;
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
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
          delay_fixed_layout_deferred();
        }
      } else if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1)) {
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (keys & KEY_TOUCH) {
      touchPosition mapped = TouchRead();
      pdf_touch_drag_active_ = true;
      bookcurrent_->SetFixedLayoutViewportInteraction(true);
      pdf_touch_last_x_ = (int)mapped.px;
      pdf_touch_last_y_ = (int)mapped.py;
      if (bookcurrent_->MoveFixedLayoutViewportToPreview((int)mapped.px,
                                                         (int)mapped.py)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
    } else if (held & KEY_TOUCH) {
      touchPosition mapped = TouchRead();
      if (!pdf_touch_drag_active_ ||
          pdf_view_utils::TouchMovementExceedsThreshold(
              pdf_touch_last_x_, pdf_touch_last_y_, (int)mapped.px,
              (int)mapped.py, kPdfTouchRerenderDelta)) {
        pdf_touch_drag_active_ = true;
        bookcurrent_->SetFixedLayoutViewportInteraction(true);
        pdf_touch_last_x_ = (int)mapped.px;
        pdf_touch_last_y_ = (int)mapped.py;
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
      ShowLibraryView();
      prefs->Write();
    } else if (keys & KEY_SELECT) {
      ShowSettingsView(true);
      prefs->Write();
    }

    if (!(held & KEY_TOUCH)) {
      if (pdf_touch_drag_active_) {
        bookcurrent_->SetFixedLayoutViewportInteraction(false);
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
        delay_fixed_layout_deferred();
      }
      bookcurrent_->SetFixedLayoutViewportInteraction(false);
      pdf_touch_drag_active_ = false;
      pdf_touch_last_x_ = -1;
      pdf_touch_last_y_ = -1;
    }

    if (status_dirty) {
      RequestStatusRedraw();
    } else if (!(held & KEY_TOUCH) && keys == 0 &&
               bookcurrent_->HasPendingFixedLayoutDeferredWork() &&
               osGetTime() >= pdf_deferred_ready_at_ms_) {
      const u32 budget_ms = 4;
      const bool worked = bookcurrent_->PumpDeferredFixedLayoutWork(budget_ms);
      const u32 delay_ms = bookcurrent_->GetFixedLayoutDeferredDelayMs();
      pdf_deferred_ready_at_ms_ = delay_ms ? (osGetTime() + delay_ms) : 0;
      if (worked) {
        DrawBookPage(bookcurrent_, ts);
        RequestStatusRedraw();
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
    touchPosition mapped = TouchRead();
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
    ShowLibraryView();
    prefs->Write();
  } else if (keys & KEY_SELECT) {
    // Go directly to settings from book.
    ShowSettingsView(true);
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
      if (now >= mobi_deferred_ready_at_ms_) {
        PumpDeferredMobi(bookcurrent_, kMobiDeferredIdleBudgetMs,
                         kMobiDeferredIdlePageBudget, &pagecount,
                         &status_dirty, &deferred_pumped);
        mobi_deferred_ready_at_ms_ =
            bookcurrent_->HasDeferredMobiParse()
                ? (osGetTime() + kMobiDeferredIdleDelayMs)
                : 0;
      }
    } else {
      mobi_deferred_ready_at_ms_ = 0;
    }
  }

  if (MaybeFinalizeDeferredRelayout(bookcurrent_, pagecount)) {
    DrawBookPage(bookcurrent_, ts);
    status_dirty = true;
  }

  if (status_dirty)
    RequestStatusRedraw();
}

void App::ToggleBookmark() {
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
  pdf_deferred_ready_at_ms_ = delay_ms ? (osGetTime() + delay_ms) : 0;
  RequestStatusRedraw();
}

void App::CloseBook() {
  if (bookcurrent_) {
    bookcurrent_->Close();
    bookcurrent_ = NULL;
  }
  pdf_deferred_ready_at_ms_ = 0;
  mobi_deferred_ready_at_ms_ = 0;
  ClearDeferredRelayoutState();
}

int App::GetBookIndex(Book *b) {
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

u8 App::OpenBook(void) {
  //! Attempt to open book indicated by bookselected.

  if (!browser_.selected_book)
    return 254;

  DBG_LOG(this, "opening book ...");
  if (browser_.selected_book->GetTitle())
    DBG_LOG(this, browser_.selected_book->GetTitle());

  const bool needs_relayout = BookNeedsRelayout(browser_.selected_book);

  // Fast path: selected book is already parsed and resident.
  if (browser_.selected_book->GetPageCount() > 0 && !needs_relayout) {
    ReuseParsedBook(this);
    if (deferred_relayout_.book != bookcurrent_)
      ClearDeferredRelayoutState();
    mobi_deferred_ready_at_ms_ =
        (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
            ? (osGetTime() + kMobiDeferredIdleDelayMs)
            : 0;
    RequestStatusRedraw();
    prefs_view_.layout_notice_pending = false;
    prefs->Write();
    return 0;
  }

  OpenBookRelayoutState relayout_state =
      CaptureRelayoutState(browser_.selected_book, needs_relayout);
  ClearDeferredRelayoutState();

  // While parsing a new book, avoid displaying stale browser highlight state.
  DrawOpeningSplash(this);

  if (browser_.selected_book->SupportsAsyncReflowOpen()) {
    if (GetCurrentBook() && GetCurrentBook() != browser_.selected_book)
      GetCurrentBook()->Close();
    if (browser_.selected_book->StartAsyncReflowOpen()) {
      opening_.pending = true;
      opening_.book = browser_.selected_book;
      opening_.needs_relayout = relayout_state.needs_relayout;
      opening_.old_page_count = relayout_state.old_page_count;
      opening_.old_position = relayout_state.old_position;
      opening_.old_bookmarks = relayout_state.old_bookmarks;
      opening_.started_at_ms = osGetTime();
      DBG_LOGF(this, "REFLOW: async open submitted book=%s",
               browser_.selected_book->GetFileName()
                   ? browser_.selected_book->GetFileName()
                   : "");
      mode_ = AppMode::Opening;
      return 0;
    }
    DBG_LOGF(this, "REFLOW: async open fallback book=%s",
             browser_.selected_book->GetFileName()
                 ? browser_.selected_book->GetFileName()
                 : "");
  }

  if (u8 err = OpenSelectedBook(this))
    return err;
  bookcurrent_ = browser_.selected_book;
  // Remember which layout generation produced these pages.
  bookcurrent_->SetLayoutRevision(layout_revision);

  int pageCount = bookcurrent_->GetPageCount();
  DBG_LOGF(this, "Generated %d pages", pageCount);

  if (pageCount <= 0) {
    PrintStatus("error: book has no parsed pages");
    bookcurrent_->Close();
    bookcurrent_ = nullptr;
    mode_ = AppMode::Browser;
    browser_.view_dirty = true;
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
    deferred_relayout_.pending = true;
    deferred_relayout_.book = bookcurrent_;
    deferred_relayout_.old_page_count = relayout_state.old_page_count;
    deferred_relayout_.old_position = relayout_state.old_position;
    deferred_relayout_.old_bookmarks = relayout_state.old_bookmarks;
    deferred_relayout_.initial_position = open_plan.mapped_position;
  } else {
    ClearDeferredRelayoutState();
  }

  EnsureBookMode(this, "OpenBook: switched mode to APP_MODE_BOOK");

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  DrawBookPage(bookcurrent_, ts);
  if (bookcurrent_ && bookcurrent_->IsFixedLayout())
    pdf_deferred_ready_at_ms_ =
        bookcurrent_->HasPendingFixedLayoutDeferredWork()
            ? (osGetTime() + bookcurrent_->GetFixedLayoutDeferredDelayMs())
            : 0;
  mobi_deferred_ready_at_ms_ =
      (bookcurrent_ && bookcurrent_->HasDeferredMobiParse())
          ? (osGetTime() + kMobiDeferredIdleDelayMs)
          : 0;
  RequestStatusRedraw();
  prefs_view_.layout_notice_pending = false;
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
