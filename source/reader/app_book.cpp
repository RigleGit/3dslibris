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

void DrawBookPage(Book *book, Text *ts) {
  if (!book || !ts || book->GetPageCount() == 0)
    return;
  book->DrawCurrentView(ts);
}

bool SetBookPage(Book *book, Text *ts, u16 page) {
  if (!book || !ts || page >= book->GetPageCount())
    return false;
  book->SetPosition(page);
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
    app->ts->SetPen(12, 50);
    app->ts->PrintString(name);
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

void ApplyRelayoutState(Book *book, const OpenBookRelayoutState &state,
                        int pageCount) {
  if (!book || !state.needs_relayout)
    return;
  book->SetPosition(layout_reflow::RemapPageIndexApprox(
      state.old_position, state.old_page_count, pageCount));
  ApplyRemappedBookmarks(
      book, layout_reflow::RemapBookmarksApprox(state.old_bookmarks,
                                                state.old_page_count,
                                                pageCount));
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

void App::HandleEventInBook() {
  u16 pagecurrent = bookcurrent_->GetPosition();
  u16 pagecount = bookcurrent_->GetPageCount();
  bool status_dirty = false;
  bool deferred_pumped = false;

  // Use 3DS edge-triggered key state to avoid carry-over/repeat from the key
  // press used to open the book.
  u32 keys = hidKeysDown();
  u32 held = hidKeysHeld();

  if (bookcurrent_ && bookcurrent_->IsPdf()) {
    if (keys & KEY_A) {
      if (bookcurrent_->ChangePdfZoom(1)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
      }
    } else if (keys & KEY_B) {
      if (bookcurrent_->ChangePdfZoom(-1)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
      }
    } else if (keys & (key.right | KEY_RIGHT)) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, 1))
        status_dirty = true;
    } else if (keys & (key.left | KEY_LEFT)) {
      if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1))
        status_dirty = true;
    } else if (keys & (key.down | KEY_DOWN)) {
      if (!bookcurrent_->GetChapters().empty()) {
        if (bookcurrent_->JumpPdfChapter(1)) {
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
        }
      } else if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, 1)) {
        status_dirty = true;
      }
    } else if (keys & (key.up | KEY_UP)) {
      if (!bookcurrent_->GetChapters().empty()) {
        if (bookcurrent_->JumpPdfChapter(-1)) {
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
        }
      } else if (TurnBookPage(bookcurrent_, ts, &pagecurrent, pagecount, -1)) {
        status_dirty = true;
      }
    } else if (keys & KEY_TOUCH) {
      touchPosition mapped = TouchRead();
      pdf_touch_drag_active_ = true;
      pdf_touch_last_x_ = (int)mapped.px;
      pdf_touch_last_y_ = (int)mapped.py;
      if (bookcurrent_->MovePdfViewportToPreview((int)mapped.px,
                                                 (int)mapped.py)) {
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
      }
    } else if (held & KEY_TOUCH) {
      touchPosition mapped = TouchRead();
      if (!pdf_touch_drag_active_ ||
          pdf_view_utils::TouchMovementExceedsThreshold(
              pdf_touch_last_x_, pdf_touch_last_y_, (int)mapped.px,
              (int)mapped.py, kPdfTouchRerenderDelta)) {
        pdf_touch_drag_active_ = true;
        pdf_touch_last_x_ = (int)mapped.px;
        pdf_touch_last_y_ = (int)mapped.py;
        if (bookcurrent_->MovePdfViewportToPreview((int)mapped.px,
                                                   (int)mapped.py)) {
          // DrawCurrentView now uses the full-page zoom cache for viewport
          // extraction (no MuPDF re-render), so updating both screens is fast.
          DrawBookPage(bookcurrent_, ts);
          status_dirty = true;
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
        DrawBookPage(bookcurrent_, ts);
        status_dirty = true;
      }
      pdf_touch_drag_active_ = false;
      pdf_touch_last_x_ = -1;
      pdf_touch_last_y_ = -1;
    }

    if (status_dirty)
      RequestStatusRedraw();
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

  if (!deferred_pumped)
    PumpDeferredMobi(bookcurrent_, 4, 1, &pagecount, &status_dirty,
                     &deferred_pumped);

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
  RequestStatusRedraw();
}

void App::CloseBook() {
  if (!bookcurrent_)
    return;
  bookcurrent_->Close();
  bookcurrent_ = NULL;
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
    RequestStatusRedraw();
    prefs_view_.layout_notice_pending = false;
    prefs->Write();
    return 0;
  }

  OpenBookRelayoutState relayout_state =
      CaptureRelayoutState(browser_.selected_book, needs_relayout);

  // While parsing a new book, avoid displaying stale browser highlight state.
  DrawOpeningSplash(this);

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
  ApplyRelayoutState(bookcurrent_, relayout_state, pageCount);

  EnsureBookMode(this, "OpenBook: switched mode to APP_MODE_BOOK");

  if (bookcurrent_->GetPosition() >= pageCount)
    bookcurrent_->SetPosition(0);
  DrawBookPage(bookcurrent_, ts);
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
