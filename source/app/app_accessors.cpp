/*
    3dslibris - app_accessors.cpp
    Extracted from app.cpp to reduce the size of the App god-file.
    Contains trivial getters/setters and small forwarders for opening state,
    deferred relayout state, session IDs, PDF touch tracking, inline-link
    interaction, lifecycle queries, and page-repeat helpers.

    No behavior change — pure code motion.
*/

#include "app/app.h"

#include "app/library_controller.h"
#include "settings/prefs.h"

bool App::IsFontMode(AppMode mode)
{
  return mode == AppMode::PrefsFont || mode == AppMode::PrefsFontBold ||
         mode == AppMode::PrefsFontItalic ||
         mode == AppMode::PrefsFontBoldItalic;
}

AppMode App::GetMode() const { return nav_.mode; }

void App::SetMode(AppMode mode) { nav_.mode = mode; }

Book *App::GetSelectedBook() const { return nav_.browser.selected_book; }

void App::SetSelectedBook(Book *book) { nav_.browser.selected_book = book; }

Book *App::GetCurrentBook() const { return reader_state_.bookcurrent; }

void App::SetCurrentBook(Book *book) { reader_state_.bookcurrent = book; }

int App::BookCount() const { return (int)books.size(); }

int App::GetSelectedBookIndex() const
{
  if (!nav_.browser.selected_book)
    return -1;
  for (size_t i = 0; i < books.size(); i++)
  {
    if (books[i] == nav_.browser.selected_book)
      return (int)i;
  }
  return -1;
}

int App::GetBrowserPageStart() const { return nav_.browser.page_start; }

void App::SetBrowserPageStart(int page_start)
{
  if (page_start < 0)
    page_start = 0;
  nav_.browser.page_start = page_start;
}

u64 App::GetBrowserLastInteractionMs() const
{
  return nav_.browser.last_interaction_ms;
}

void App::SetBrowserLastInteractionMs(u64 ms)
{
  nav_.browser.last_interaction_ms = ms;
}

bool App::IsBrowserWaitingInputRelease() const
{
  return nav_.browser.wait_input_release;
}

void App::SetBrowserWaitingInputRelease(bool wait_input_release)
{
  nav_.browser.wait_input_release = wait_input_release;
}

void App::SetBrowserDirty(bool dirty) { nav_.browser.view_dirty = dirty; }

void App::MarkBrowserDirty() { nav_.browser.view_dirty = true; }

int App::GetPrefsSelectedIndex() const { return nav_.prefs.selected_index; }

void App::SetPrefsSelectedIndex(int selected_index)
{
  nav_.prefs.selected_index = selected_index;
}

void App::SetBookSettingsContext(bool from_book)
{
  nav_.prefs.from_book = from_book;
}

bool App::IsPrefsLayoutNoticePending() const
{
  return nav_.prefs.layout_notice_pending;
}

void App::SetPrefsLayoutNoticePending(bool pending)
{
  nav_.prefs.layout_notice_pending = pending;
}

void App::SetPrefsDirty(bool dirty) { nav_.prefs.view_dirty = dirty; }

void App::MarkPrefsDirty() { nav_.prefs.view_dirty = true; }

bool App::IsPrefsDirty() const { return nav_.prefs.view_dirty; }

bool App::IsBrowserDirty() const { return nav_.browser.view_dirty; }

bool App::ShouldSkipNextBrowserPresent() const
{
  return skip_next_browser_present_;
}

void App::ClearSkipNextBrowserPresent() { skip_next_browser_present_ = false; }

void App::PersistPrefs() { prefs->Write(); }

int App::StartupFindBooks() { return library_controller_->FindBooks(); }

void App::StartupPrepareLibrary() { library_controller_->PrepareLibrary(); }

void App::StartupInitUiAndBrowser()
{
  PrefsInit();
  browser_init();
  SetBrowserDirty(true);
}

void App::StartupInitScreens() { InitScreens(); }

bool App::HasPendingBootReopen() const { return pending_boot_reopen_; }

void App::SetPendingBootReopen(bool v) { pending_boot_reopen_ = v; }

bool App::IsOpeningPending() const { return reader_state_.opening.pending; }

void App::SetOpeningPending(bool pending) { reader_state_.opening.pending = pending; }

Book *App::GetOpeningBook() const { return reader_state_.opening.book; }

void App::SetOpeningBook(Book *book) { reader_state_.opening.book = book; }

unsigned int App::GetOpeningSessionId() const
{
  return reader_state_.opening.session_id;
}

void App::SetOpeningSessionId(unsigned int session_id)
{
  reader_state_.opening.session_id = session_id;
}

bool App::IsOpeningNeedsRelayout() const
{
  return reader_state_.opening.needs_relayout;
}

void App::SetOpeningNeedsRelayout(bool needs_relayout)
{
  reader_state_.opening.needs_relayout = needs_relayout;
}

int App::GetOpeningOldPageCount() const { return reader_state_.opening.old_page_count; }

void App::SetOpeningOldPageCount(int old_page_count)
{
  reader_state_.opening.old_page_count = old_page_count;
}

int App::GetOpeningOldPosition() const { return reader_state_.opening.old_position; }

void App::SetOpeningOldPosition(int old_position)
{
  reader_state_.opening.old_position = old_position;
}

unsigned int App::GetOpeningSpineDone() const
{
  return __atomic_load_n(&reader_state_.opening.spine_done, __ATOMIC_ACQUIRE);
}

unsigned int App::GetOpeningSpineTotal() const
{
  return __atomic_load_n(&reader_state_.opening.spine_total, __ATOMIC_ACQUIRE);
}

void App::SetOpeningSpineProgress(unsigned int done, unsigned int total)
{
  __atomic_store_n(&reader_state_.opening.spine_done, done, __ATOMIC_RELEASE);
  __atomic_store_n(&reader_state_.opening.spine_total, total, __ATOMIC_RELEASE);
  __atomic_add_fetch(&reader_state_.opening.progress_seq, 1u, __ATOMIC_ACQ_REL);
}

unsigned int App::GetOpeningProgressSeq() const
{
  return __atomic_load_n(&reader_state_.opening.progress_seq, __ATOMIC_ACQUIRE);
}

unsigned int App::GetOpeningDrawnProgressSeq() const
{
  return reader_state_.opening.drawn_progress_seq;
}

void App::SetOpeningDrawnProgressSeq(unsigned int seq)
{
  reader_state_.opening.drawn_progress_seq = seq;
}

std::list<int> &App::MutableOpeningOldBookmarks()
{
  return reader_state_.opening.old_bookmarks;
}

u64 App::GetOpeningStartedAtMs() const { return reader_state_.opening.started_at_ms; }

void App::SetOpeningStartedAtMs(u64 started_at_ms)
{
  reader_state_.opening.started_at_ms = started_at_ms;
}

bool App::IsDeferredRelayoutPending() const
{
  return reader_state_.deferred_relayout.pending;
}

void App::SetDeferredRelayoutPending(bool pending)
{
  reader_state_.deferred_relayout.pending = pending;
}

Book *App::GetDeferredRelayoutBook() const
{
  return reader_state_.deferred_relayout.book;
}

void App::SetDeferredRelayoutBook(Book *book)
{
  reader_state_.deferred_relayout.book = book;
}

int App::GetDeferredRelayoutOldPageCount() const
{
  return reader_state_.deferred_relayout.old_page_count;
}

void App::SetDeferredRelayoutOldPageCount(int old_page_count)
{
  reader_state_.deferred_relayout.old_page_count = old_page_count;
}

int App::GetDeferredRelayoutOldPosition() const
{
  return reader_state_.deferred_relayout.old_position;
}

void App::SetDeferredRelayoutOldPosition(int old_position)
{
  reader_state_.deferred_relayout.old_position = old_position;
}

std::list<int> &App::MutableDeferredRelayoutOldBookmarks()
{
  return reader_state_.deferred_relayout.old_bookmarks;
}

int App::GetDeferredRelayoutInitialPosition() const
{
  return reader_state_.deferred_relayout.initial_position;
}

void App::SetDeferredRelayoutInitialPosition(int initial_position)
{
  reader_state_.deferred_relayout.initial_position = initial_position;
}

unsigned int App::GetCurrentBookSessionId() const
{
  return reader_state_.current_book_session_id;
}

void App::SetCurrentBookSessionId(unsigned int session_id)
{
  reader_state_.current_book_session_id = session_id;
}

unsigned int App::AllocateBookSessionId()
{
  return reader_state_.next_book_session_id++;
}

unsigned int App::GetLayoutRevision() const { return reader_state_.layout_revision; }

void App::SetLayoutRevision(unsigned int layout_revision)
{
  reader_state_.layout_revision = layout_revision;
}

bool App::IsPdfTouchDragActive() const { return reader_state_.pdf_touch_drag_active; }

void App::SetPdfTouchDragActive(bool active)
{
  reader_state_.pdf_touch_drag_active = active;
}

int App::GetPdfTouchLastX() const { return reader_state_.pdf_touch_last_x; }

void App::SetPdfTouchLastX(int x) { reader_state_.pdf_touch_last_x = x; }

int App::GetPdfTouchLastY() const { return reader_state_.pdf_touch_last_y; }

void App::SetPdfTouchLastY(int y) { reader_state_.pdf_touch_last_y = y; }

u64 App::GetPdfDeferredReadyAtMs() const
{
  return reader_state_.pdf_deferred_ready_at_ms;
}

void App::SetPdfDeferredReadyAtMs(u64 ready_at_ms)
{
  reader_state_.pdf_deferred_ready_at_ms = ready_at_ms;
}

bool App::IsInlineLinkFocusActive() const
{
  return reader_state_.inline_link_focus_active;
}

void App::SetInlineLinkFocusActive(bool active)
{
  reader_state_.inline_link_focus_active = active;
}

bool App::IsInlineLinkHoldArmed() const
{
  return reader_state_.inline_link_hold_armed;
}

void App::SetInlineLinkHoldArmed(bool armed)
{
  reader_state_.inline_link_hold_armed = armed;
}

bool App::IsInlineLinkHoldConsumed() const
{
  return reader_state_.inline_link_hold_consumed;
}

void App::SetInlineLinkHoldConsumed(bool consumed)
{
  reader_state_.inline_link_hold_consumed = consumed;
}

u64 App::GetInlineLinkHoldStartedAtMs() const
{
  return reader_state_.inline_link_hold_started_at_ms;
}

void App::SetInlineLinkHoldStartedAtMs(u64 started_at_ms)
{
  reader_state_.inline_link_hold_started_at_ms = started_at_ms;
}

bool App::IsNew3dsDevice() const { return lifecycle_state_.IsNew3DS(); }

bool App::IsHomebrewEnvironment() const { return lifecycle_state_.IsHomebrew(); }

bool App::IsAppletSuspended() const { return lifecycle_state_.IsSuspended(); }

bool App::ShouldAbortWork() const
{
  return lifecycle_state_.ShouldAbortWork(static_cast<u8>(nav_.mode));
}

void App::ResetPageRepeat()
{
  reader::ResetPageRepeat(&reader_state_.page_repeat);
}

bool App::ShouldFirePageRepeat(reader::PageRepeatAction action, bool down_now,
                               bool held_now, u64 now_ms,
                               u64 initial_delay_ms,
                               u64 repeat_interval_ms)
{
  return reader::ShouldFirePageRepeat(&reader_state_.page_repeat, action,
                                      down_now, held_now, now_ms,
                                      initial_delay_ms, repeat_interval_ms);
}
