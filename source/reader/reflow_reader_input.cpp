/*
    3dslibris - reflow_reader_input.cpp
    New 3DS reader module by Rigle.

    Summary:
    - Input handling for reflowable books (EPUB/FB2/MOBI/TXT etc.) in reading mode.
    - Extracted from reader/app_book.cpp HandleEventInBook reflowable branch.
    - Inline link interaction helpers (EnterInlineLinkFocus, TryFollowTouchLink,
      etc.) are private to this translation unit.
*/

#include "reader/reflow_reader_input.h"

#include <3ds.h>

#include "app/app.h"
#include "book/book.h"
#include "book/page.h"
#include "reader/book_page_nav.h"
#include "reader/inline_link_utils.h"
#include "reader/page_repeat_utils.h"
#include "settings/prefs.h"
#include "shared/app_flow_utils.h"
#include "ui/text.h"
#include "ui/ui_button_skin.h"

namespace {

static const uint64_t kInlineLinkHoldThresholdMs = 350;
static const uint64_t kPageRepeatInitialDelayMs = 400;
static const uint64_t kPageRepeatIntervalMs = 150;
static const int kInlineLinkSecondScreenYOffset = 420;
static const int kTouchLinkMinHitPx = 12;
static const int kTouchLinkPadPx = 4;

static const std::vector<Page::InlineLinkRenderEntry> *
CurrentPageInlineLinks(Book *book) {
  if (!book || book->GetPageCount() == 0 || book->IsFixedLayout())
    return NULL;
  Page *page = book->GetPage();
  if (!page)
    return NULL;
  return &page->GetRenderedInlineLinks();
}

static bool CurrentPageHasInlineLinks(Book *book) {
  const std::vector<Page::InlineLinkRenderEntry> *links =
      CurrentPageInlineLinks(book);
  return links && !links->empty();
}

static void ExitInlineLinkFocus(App *app, Book *book) {
  if (book)
    book->ClearFocusedInlineLink();
  if (app)
    app->SetInlineLinkFocusActive(false);
}

static bool EnterInlineLinkFocus(App *app, Book *book, Text *ts) {
  if (!app || !book || !ts)
    return false;
  const std::vector<Page::InlineLinkRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;
  int focus_index = book->GetFocusedInlineLinkIndex();
  if (focus_index < 0 || focus_index >= (int)links->size())
    focus_index = 0;
  book->SetFocusedInlineLinkIndex(focus_index);
  app->SetInlineLinkFocusActive(true);
  book_nav::DrawPage(book, ts);
  return true;
}

static size_t InlineLinkCountForPage(Book *book, int page_index) {
  if (!book || book->IsFixedLayout() || page_index < 0 ||
      page_index >= book->GetPageCount())
    return 0;
  Page *page = book->GetPage(page_index);
  return page ? page->GetInlineLinkCount() : 0;
}

static bool MoveInlineLinkFocusSequential(Book *book, Text *ts, int direction) {
  if (!book || !ts || direction == 0 || book->IsFixedLayout())
    return false;
  const std::vector<Page::InlineLinkRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;

  int current_index = book->GetFocusedInlineLinkIndex();
  if (current_index < 0 || current_index >= (int)links->size())
    current_index = 0;

  if (direction > 0 && current_index + 1 < (int)links->size()) {
    book->SetFocusedInlineLinkIndex(current_index + 1);
    book_nav::DrawPage(book, ts);
    return true;
  }
  if (direction < 0 && current_index > 0) {
    book->SetFocusedInlineLinkIndex(current_index - 1);
    book_nav::DrawPage(book, ts);
    return true;
  }

  const int page_count = (int)book->GetPageCount();
  const int current_page = book->GetPosition();
  if (direction > 0) {
    for (int page_index = current_page + 1; page_index < page_count; ++page_index) {
      const size_t link_count = InlineLinkCountForPage(book, page_index);
      if (link_count == 0)
        continue;
      book->SetPosition(page_index);
      book->SetFocusedInlineLinkIndex(0);
      book_nav::DrawPage(book, ts);
      return true;
    }
    return false;
  }

  for (int page_index = current_page - 1; page_index >= 0; --page_index) {
    const size_t link_count = InlineLinkCountForPage(book, page_index);
    if (link_count == 0)
      continue;
    book->SetPosition(page_index);
    book->SetFocusedInlineLinkIndex((int)link_count - 1);
    book_nav::DrawPage(book, ts);
    return true;
  }
  return false;
}

static bool FollowFocusedInlineLink(Book *book, Text *ts) {
  if (!book || !ts)
    return false;
  const std::vector<Page::InlineLinkRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;
  const int focus_index = book->GetFocusedInlineLinkIndex();
  if (focus_index < 0 || focus_index >= (int)links->size())
    return false;
  const uint16_t href_id = (*links)[(size_t)focus_index].href_id;
  const std::string *href = book->GetInlineLinkHref(href_id);
  if (!href || href->empty())
    return false;
  uint16_t target_page = 0;
  bool found_anchor = book->FindChapterAnchorPage(*href, &target_page);
  bool found_doc = false;
  if (!found_anchor)
    found_doc = book->FindChapterDocStartPage(*href, &target_page);
  if (!found_anchor && !found_doc)
    return false;
  book->ClearFocusedInlineLink();
  return book_nav::SetPage(book, ts, target_page);
}

static bool TryFollowTouchLink(Book *book, Text *ts, int tx, int ty) {
  if (!book || !ts)
    return false;
  const std::vector<Page::InlineLinkRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;

  int best_index = -1;
  for (int i = 0; i < (int)links->size(); ++i) {
    const Page::InlineLinkRenderEntry &entry = (*links)[(size_t)i];
    if (entry.screen_index != 1)
      continue;

    inline_link_utils::LinkRect r = entry.bounds;
    const int w = r.x1 - r.x0;
    const int h = r.y1 - r.y0;
    const int cx = r.x0 + w / 2;
    const int cy = r.y0 + h / 2;
    if (w < kTouchLinkMinHitPx) {
      r.x0 = cx - kTouchLinkMinHitPx / 2;
      r.x1 = cx + kTouchLinkMinHitPx / 2;
    }
    if (h < kTouchLinkMinHitPx) {
      r.y0 = cy - kTouchLinkMinHitPx / 2;
      r.y1 = cy + kTouchLinkMinHitPx / 2;
    }
    r.x0 -= kTouchLinkPadPx;
    r.y0 -= kTouchLinkPadPx;
    r.x1 += kTouchLinkPadPx;
    r.y1 += kTouchLinkPadPx;

    if (tx >= r.x0 && tx < r.x1 && ty >= r.y0 && ty < r.y1) {
      best_index = i;
      break;
    }
  }

  if (best_index < 0)
    return false;

  book->SetFocusedInlineLinkIndex(best_index);
  return FollowFocusedInlineLink(book, ts);
}

} // namespace

namespace reflow_input {

bool HandleInBook(App &app, Book *book, Text *ts, Prefs * /*prefs*/,
                  uint32_t keys, uint32_t held, uint16_t *pagecurrent,
                  uint16_t *pagecount, const ReaderControls &ctrl) {
  bool status_dirty = false;
  const bool has_inline_links = CurrentPageHasInlineLinks(book);
  const uint64_t now_ms = osGetTime();

  if (!app.IsInlineLinkFocusActive() && (keys & app.key.y)) {
    app.SetInlineLinkHoldArmed(true);
    app.SetInlineLinkHoldConsumed(false);
    app.SetInlineLinkHoldStartedAtMs(now_ms);
  }

  if (!app.IsInlineLinkFocusActive() && app.IsInlineLinkHoldArmed() &&
      (held & app.key.y) && !app.IsInlineLinkHoldConsumed() &&
      has_inline_links &&
      now_ms >= app.GetInlineLinkHoldStartedAtMs() + kInlineLinkHoldThresholdMs) {
    if (EnterInlineLinkFocus(&app, book, ts)) {
      app.SetInlineLinkHoldConsumed(true);
      status_dirty = true;
    }
  }

  if (app.IsInlineLinkFocusActive()) {
    if (keys & (app.key.b | app.key.y)) {
      ExitInlineLinkFocus(&app, book);
      book_nav::DrawPage(book, ts);
      status_dirty = true;
    } else if (keys & app.key.a) {
      if (FollowFocusedInlineLink(book, ts)) {
        app.SetInlineLinkFocusActive(false);
        status_dirty = true;
      }
    } else if (keys & ctrl.link_next) {
      if (MoveInlineLinkFocusSequential(book, ts, 1))
        status_dirty = true;
    } else if (keys & ctrl.link_prev) {
      if (MoveInlineLinkFocusSequential(book, ts, -1))
        status_dirty = true;
    } else if (keys & ctrl.back_to_library) {
      ExitInlineLinkFocus(&app, book);
      app.ShowLibraryView();
      app.prefs->Write();
    } else if (keys & ctrl.open_settings) {
      ExitInlineLinkFocus(&app, book);
      app.ShowSettingsView(true);
      app.prefs->Write();
    }
  } else {
    // D-pad up/down support page repeat: holding the key fires repeated page
    // turns after an initial delay. Other page-turn keys (A, R, shoulder) do
    // not repeat.
    const uint32_t dpad_repeat_keys = app.key.ddown | app.key.dup;
    const uint32_t non_repeat_held = held & ~dpad_repeat_keys;
    const uint32_t non_repeat_keys = keys & ~dpad_repeat_keys;
    bool repeat_next = false;
    bool repeat_prev = false;
    if (non_repeat_held == 0 && non_repeat_keys == 0 && (held & app.key.ddown)) {
      repeat_next = app.ShouldFirePageRepeat(
          reader::PAGE_REPEAT_NEXT, (keys & app.key.ddown) != 0,
          (held & app.key.ddown) != 0, now_ms,
          kPageRepeatInitialDelayMs, kPageRepeatIntervalMs);
    } else if (non_repeat_held == 0 && non_repeat_keys == 0 && (held & app.key.dup)) {
      repeat_prev = app.ShouldFirePageRepeat(
          reader::PAGE_REPEAT_PREVIOUS, (keys & app.key.dup) != 0,
          (held & app.key.dup) != 0, now_ms,
          kPageRepeatInitialDelayMs, kPageRepeatIntervalMs);
    } else if ((held & dpad_repeat_keys) == 0 || non_repeat_held != 0) {
      app.ResetPageRepeat();
    }

    if ((keys & ctrl.page_next) || repeat_next) {
      if (!book_nav::AdvancePage(book, ts, pagecurrent, pagecount, &status_dirty))
        app.ResetPageRepeat();
    } else if ((keys & ctrl.page_prev) || repeat_prev) {
      if (book_nav::TurnPage(book, ts, pagecurrent, *pagecount, -1))
        status_dirty = true;
      else
        app.ResetPageRepeat();
    } else if (keys & app.key.x) {
      int mode = ts->GetColorMode();
      int next = (mode + 1) % 6;
      app.colorMode = next;
      ts->SetColorMode(next);
      UiButtonSkin_SetColorMode(next);
      ts->MarkAllScreensDirty();
      book_nav::DrawPage(book, ts);
      status_dirty = true;
    } else if (keys & app.key.y) {
      // y without hold: consumed by hold-arm; no-op here
    } else if (keys & KEY_TOUCH) {
      app.ResetPageRepeat();
      touchPosition mapped = app.TouchRead();
      if (TryFollowTouchLink(book, ts, (int)mapped.px, (int)mapped.py)) {
        status_dirty = true;
      } else {
        const bool forward_zone = ((int)mapped.px >= 120);
        if (!forward_zone) {
          if (book_nav::TurnPage(book, ts, pagecurrent, *pagecount, -1))
            status_dirty = true;
        } else {
          book_nav::AdvancePage(book, ts, pagecurrent, pagecount, &status_dirty);
        }
      }
    } else if (keys & ctrl.back_to_library) {
      app.ResetPageRepeat();
      app.ShowLibraryView();
      app.prefs->Write();
    } else if (keys & ctrl.open_settings) {
      app.ResetPageRepeat();
      app.ShowSettingsView(true);
      app.prefs->Write();
    } else if (keys & (ctrl.bookmark_prev | ctrl.bookmark_next)) {
      app.ResetPageRepeat();
      app_flow_utils::BookmarkJumpResult jump = app_flow_utils::FindBookmarkJumpTarget(
          book->GetBookmarks(), book->GetPosition(),
          (keys & ctrl.bookmark_prev)
              ? app_flow_utils::BookmarkJumpDirection::Previous
              : app_flow_utils::BookmarkJumpDirection::Next);
      if (jump.found) {
        book->SetPosition(jump.page);
        book_nav::DrawPage(book, ts);
        status_dirty = true;
      }
    }
  } // end else (reflowable input)

  if (app.IsInlineLinkHoldArmed() && !(held & app.key.y)) {
    const bool consumed = app.IsInlineLinkHoldConsumed();
    app.SetInlineLinkHoldArmed(false);
    app.SetInlineLinkHoldStartedAtMs(0);
    app.SetInlineLinkHoldConsumed(false);
    if (!consumed && !app.IsInlineLinkFocusActive())
      app.ToggleBookmark();
  }

  return status_dirty;
}

} // namespace reflow_input
