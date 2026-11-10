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
#include "shared/orientation_utils.h"
#include "ui/text.h"
#include "ui/ui_button_skin.h"

namespace {

static const uint64_t kInlineLinkHoldThresholdMs = 350;
static const uint64_t kPageRepeatInitialDelayMs = 400;
static const uint64_t kPageRepeatIntervalMs = 150;
static const int kInlineLinkSecondScreenYOffset = 420;
static const int kTouchLinkMinHitPx = 12;
static const int kTouchLinkPadPx = 4;

static const std::vector<Page::SelectableRenderEntry> *
CurrentPageInlineLinks(Book *book) {
  if (!book || book->GetPageCount() == 0 || book->IsFixedLayout())
    return NULL;
  Page *page = book->GetPage();
  if (!page)
    return NULL;
  return &page->GetRenderedSelectables();
}

static bool CurrentPageHasInlineLinks(Book *book) {
  const std::vector<Page::SelectableRenderEntry> *links =
      CurrentPageInlineLinks(book);
  return links && !links->empty();
}

static void ExitInlineLinkFocus(App *app, Book *book) {
  if (book)
    book->ClearFocusedInlineLink();
  if (app)
    app->SetInlineLinkFocusActive(false);
  if (app)
    app->ResetPageRepeat();
}

static bool EnterInlineLinkFocus(App *app, Book *book, Text *ts) {
  if (!app || !book || !ts)
    return false;
  const std::vector<Page::SelectableRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;
  int focus_index = book->GetFocusedInlineLinkIndex();
  if (focus_index < 0 || focus_index >= (int)links->size())
    focus_index = 0;
  book->SetFocusedInlineLinkIndex(focus_index);
  app->SetInlineLinkFocusActive(true);
  app->ResetPageRepeat();
  book_nav::DrawPage(book, ts);
  return true;
}

static size_t InlineLinkCountForPage(Book *book, int page_index) {
  if (!book || book->IsFixedLayout() || page_index < 0 ||
      page_index >= book->GetPageCount())
    return 0;
  return book->ReflowPageLinkCount(page_index);
}

static bool MoveInlineLinkFocusSequential(Book *book, Text *ts, int direction) {
  if (!book || !ts || direction == 0 || book->IsFixedLayout())
    return false;
  const std::vector<Page::SelectableRenderEntry> *links =
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
  for (int page_index = current_page + direction;
       page_index >= 0 && page_index < page_count; page_index += direction) {
    if (InlineLinkCountForPage(book, page_index) == 0) continue;
    book->SetPosition(page_index);
    if (book->HasPendingReflowNavigation()) {
      book->SetFocusedInlineLinkIndex(
          direction > 0 ? 0 : (int)book->ReflowPageLinkCount(page_index) - 1);
      return true;
    }
    book->ClearFocusedInlineLink();
    book_nav::DrawPage(book, ts);
    const auto &targets = book->GetPage()->GetRenderedSelectables();
    if (targets.empty()) continue;
    book->SetFocusedInlineLinkIndex(direction > 0 ? 0 : (int)targets.size()-1);
    book_nav::DrawPage(book, ts);
    return true;
  }
  book->SetPosition(current_page); book->SetFocusedInlineLinkIndex(current_index);
  book_nav::DrawPage(book, ts);
  return false;
}

static bool FollowFocusedInlineLink(Book *book, Text *ts) {
  if (!book || !ts)
    return false;
  const std::vector<Page::SelectableRenderEntry> *links =
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

static bool TryTouchTarget(Book *book, Text *ts, int tx, int ty, bool selection) {
  if (!book || !ts)
    return false;
  const std::vector<Page::SelectableRenderEntry> *links =
      CurrentPageInlineLinks(book);
  if (!links || links->empty())
    return false;

  int best_index = -1;
  for (int i = 0; i < (int)links->size(); ++i) {
    const Page::SelectableRenderEntry &entry = (*links)[(size_t)i];
    const bool first_left = orientation_utils::FirstScreenIsLeft(book->GetOrientation());
    if (entry.screen_index != (first_left ? 1 : 0) ||
        (!selection && entry.href_id == 0)) continue;

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
  if (selection) { book_nav::DrawPage(book, ts); return true; }
  return FollowFocusedInlineLink(book, ts);
}

} // namespace

namespace reflow_input {

bool HandleInBook(App &app, Book *book, Text *ts, Prefs * /*prefs*/,
                  const FrameInput &input, uint16_t *pagecurrent,
                  uint16_t *pagecount, const ReaderControls &ctrl) {
  const uint32_t keys = input.keys_down;
  const uint32_t held = input.keys_held;
  bool status_dirty = false;
  const bool has_inline_links = CurrentPageHasInlineLinks(book);
  const uint64_t now_ms = input.timestamp_ms;

  if (!app.IsInlineLinkFocusActive() && (keys & ctrl.focus_hold)) {
    app.SetInlineLinkHoldArmed(true);
    app.SetInlineLinkHoldConsumed(false);
    app.SetInlineLinkHoldStartedAtMs(now_ms);
  }

  if (!app.IsInlineLinkFocusActive() && app.IsInlineLinkHoldArmed() &&
      (held & ctrl.focus_hold) && !app.IsInlineLinkHoldConsumed() &&
      has_inline_links &&
      now_ms >= app.GetInlineLinkHoldStartedAtMs() + kInlineLinkHoldThresholdMs) {
    if (EnterInlineLinkFocus(&app, book, ts)) {
      app.SetInlineLinkHoldConsumed(true);
      status_dirty = true;
    }
  }

  if (app.IsInlineLinkFocusActive()) {
    // Link focus moves repeat while a direction is held. Y is still held when
    // focus was entered by long-press, so it must not cancel the repeat.
    // Circle Pad repeat stays opt-in (analog drift protection).
    const uint32_t circle_bits =
        app.key.up | app.key.down | app.key.left | app.key.right;
    const bool circle_link_repeat =
        app.prefs.get() && app.prefs->circle_pad_page_turn;
    const uint32_t link_repeat_mask =
        circle_link_repeat ? ~0u : ~circle_bits;
    const reader::PageRepeatAction link_repeat = app.ResolvePageRepeat(
        keys, held & ~ctrl.focus_hold, ctrl.link_next & link_repeat_mask,
        ctrl.link_prev & link_repeat_mask, now_ms, kPageRepeatInitialDelayMs,
        kPageRepeatIntervalMs);
    if (keys & ctrl.link_exit) {
      ExitInlineLinkFocus(&app, book);
      book_nav::DrawPage(book, ts);
      status_dirty = true;
    } else if (keys & KEY_TOUCH) {
      touchPosition mapped = app.MapTouch(input);
      status_dirty = TryTouchTarget(book, ts, mapped.px, mapped.py, true);
    } else if (keys & ctrl.link_activate) {
      const auto *targets = CurrentPageInlineLinks(book);
      const int selected = book->GetFocusedInlineLinkIndex();
      if (targets && selected >= 0 && selected < (int)targets->size() &&
          (*targets)[selected].kind == Page::IMAGE) {
        app.OpenImageViewer();
        return false;
      }
      if (FollowFocusedInlineLink(book, ts)) {
        app.SetInlineLinkFocusActive(false);
        status_dirty = true;
      }
    } else if (keys & ctrl.link_follow_image) {
      const auto *targets = CurrentPageInlineLinks(book);
      const int selected = book->GetFocusedInlineLinkIndex();
      if (targets && selected >= 0 && selected < (int)targets->size() &&
          (*targets)[selected].kind == Page::IMAGE && FollowFocusedInlineLink(book, ts)) {
        app.SetInlineLinkFocusActive(false); status_dirty = true;
      }
    } else if ((keys & ctrl.link_next) ||
               link_repeat == reader::PAGE_REPEAT_NEXT) {
      if (MoveInlineLinkFocusSequential(book, ts, 1))
        status_dirty = true;
    } else if ((keys & ctrl.link_prev) ||
               link_repeat == reader::PAGE_REPEAT_PREVIOUS) {
      if (MoveInlineLinkFocusSequential(book, ts, -1))
        status_dirty = true;
    } else if (keys & ctrl.back_to_library) {
      ExitInlineLinkFocus(&app, book);
      app.ShowLibraryView();
    } else if (keys & ctrl.open_settings) {
      ExitInlineLinkFocus(&app, book);
      app.ShowSettingsView(true);
      app.prefs->Write();
    }
  } else {
    // Page keys support repeat (orientation-aware). Circle Pad repeat is opt-in because
    // analog drift can otherwise trigger fast accidental page turns.
    const bool circle_repeat_enabled =
        app.prefs.get() && app.prefs->circle_pad_page_turn;
    const uint32_t repeat_next_keys =
        reader_input_utils::ReflowablePageRepeatKeys(
            ctrl.page_repeat_next_circle, ctrl.page_repeat_next_dpad,
            circle_repeat_enabled);
    const uint32_t repeat_prev_keys =
        reader_input_utils::ReflowablePageRepeatKeys(
            ctrl.page_repeat_prev_circle, ctrl.page_repeat_prev_dpad,
            circle_repeat_enabled);
    const reader::PageRepeatAction repeat_action = app.ResolvePageRepeat(
        keys, held, repeat_next_keys, repeat_prev_keys, now_ms,
        kPageRepeatInitialDelayMs, kPageRepeatIntervalMs);
    const bool repeat_next = repeat_action == reader::PAGE_REPEAT_NEXT;
    const bool repeat_prev = repeat_action == reader::PAGE_REPEAT_PREVIOUS;

    if ((keys & ctrl.page_next) || repeat_next) {
      if (!book_nav::AdvancePage(book, ts, pagecurrent, pagecount, &status_dirty))
        app.ResetPageRepeat();
    } else if ((keys & ctrl.page_prev) || repeat_prev) {
      if (book_nav::TurnPage(book, ts, pagecurrent, *pagecount, -1))
        status_dirty = true;
      else
        app.ResetPageRepeat();
    } else if (keys & ctrl.color_cycle) {
      int mode = ts->GetColorMode();
      int next = (mode + 1) % 6;
      app.colorMode = next;
      ts->SetColorMode(next);
      UiButtonSkin_SetColorMode(next);
      ts->MarkAllScreensDirty();
      book_nav::DrawPage(book, ts);
      status_dirty = true;
    } else if (keys & ctrl.focus_hold) {
      // focus_hold without hold: consumed by hold-arm; no-op here
    } else if (keys & KEY_TOUCH) {
      app.ResetPageRepeat();
      touchPosition mapped = app.MapTouch(input);
      if (TryTouchTarget(book, ts, (int)mapped.px, (int)mapped.py, false)) {
        status_dirty = true;
      } else {
        const bool forward_zone =
            ((int)mapped.px >= ts->LogicalWidth() / 2);
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

  if (app.IsInlineLinkHoldArmed() && !(held & ctrl.focus_hold)) {
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
