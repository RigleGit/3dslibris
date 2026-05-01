/*
    3dslibris - app.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Replaced NDS hardware paths with 3DS/libctru equivalents.
    - Added startup flow, cover cache prep, and runtime timing telemetry.
    - Added 3DS status redraw control and bottom-screen gradient helpers.
*/

#include "app/app.h"

#include <algorithm>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>

#include "book/book.h"
#include "menus/bookmark_menu.h"
#include "ui/button.h"
#include "menus/chapter_menu.h"
#include "shared/app_flow_utils.h"
#include "settings/font.h"
#include "app/library_controller.h"
#include "app/reader_controller.h"
#include "app/settings_controller.h"
#include "app/status_controller.h"
#include "app/startup_controller.h"
#include "app/main_loop_controller.h"
#include "debug_log.h"
#include "path_utils.h"
#include "parse.h"
#include "shared/debug_runtime_mode.h"
#include "settings/prefs.h"
#include "reader/book_switch_utils.h"
#include "ui/text.h"

#ifndef ORIENTATION_DIAG
#define ORIENTATION_DIAG 0
#endif

namespace
{
  static const u64 kBrowserReturnWarmupCooldownMs = 1200;
} // end anonymous namespace
#include "color_utils.h"
#include "ui/theme_colors.h"

// Singleton instance management for App class, allowing global access to the app instance from other modules.
App *App::s_instance_ = nullptr;

App *App::GetInstance() { return s_instance_; }
void App::SetInstance(App *instance) { s_instance_ = instance; }

namespace
{

  static std::string ResolveDefaultFontDir()
  {
    return paths::GetFontDir();
  }

#if ORIENTATION_DIAG
  static int g_orientation_touch_diag_budget = 0;
#endif

  [[gnu::unused]] static const char *AppletHookName(APT_HookType hook)
  {
    switch (hook)
    {
    case APTHOOK_ONSUSPEND:
      return "suspend";
    case APTHOOK_ONRESTORE:
      return "restore";
    case APTHOOK_ONWAKEUP:
      return "wakeup";
    case APTHOOK_ONEXIT:
      return "exit";
    default:
      return "unknown";
    }
  }

} // namespace

App::App()
{
  // Initialize paths and state.
  fontdir = ResolveDefaultFontDir();
  bookdir = paths::GetBookDir();
  reader_state_.bookcurrent = nullptr;
  reopen = true; // Reopen last book on startup by default.
  nav_.mode = AppMode::Browser;
  cache = false;
  orientation = false; // Turned Left by default.
  paraspacing = 1;
  paraindent = 0;
  colorMode = 0;

  // Default key mappings
  // Circle Pad.
  key.up = KEY_CPAD_UP;
  key.down = KEY_CPAD_DOWN;
  key.left = KEY_CPAD_LEFT;
  key.right = KEY_CPAD_RIGHT;

  // D-pad.
  key.dup = KEY_DUP;
  key.ddown = KEY_DDOWN;
  key.dleft = KEY_DLEFT;
  key.dright = KEY_DRIGHT;

  // Face buttons.
  key.a = KEY_A;
  key.b = KEY_B;
  key.x = KEY_X;
  key.y = KEY_Y;

  // System buttons.
  key.start = KEY_START;
  key.select = KEY_SELECT;

  // Shoulders.
  key.l = KEY_L;
  key.r = KEY_R;
  key.zl = KEY_ZL;
  key.zr = KEY_ZR;
  key.downrepeat = key.down | key.ddown;

  // TODO: add new3ds-specific keys (c-stick) to prefs and remappable key config.

  // Initialize browser navigation state.
  nav_.browser.selected_book = nullptr;
  nav_.browser.page_start = 0;
  nav_.browser.view_dirty = false;
  nav_.browser.wait_input_release = false;
  nav_.browser.last_interaction_ms = 0;

  // Initialize controllers and other components.
  prefs = std::unique_ptr<Prefs>(new Prefs(this));
  library_controller_ = std::unique_ptr<LibraryController>(new LibraryController(*this));
  reader_controller_ = std::unique_ptr<ReaderController>(new ReaderController(*this));
  settings_controller_ = std::unique_ptr<SettingsController>(new SettingsController(*this));
  status_controller_ = std::unique_ptr<StatusController>(new StatusController(*this));
  startup_controller_ = std::unique_ptr<StartupController>(new StartupController(*this));
  main_loop_controller_ = std::unique_ptr<MainLoopController>(new MainLoopController(*this));

  // Initialize prefs view state.
  nav_.prefs.selected_index = -1;
  nav_.prefs.view_dirty = false;
  nav_.prefs.from_book = false;
  nav_.prefs.layout_notice_pending = false;

  // Initialize reader runtime state.
  reader_state_.opening = OpeningState();
  reader_state_.layout_revision = 0;
  reader_state_.pdf_touch_drag_active = false;
  reader_state_.pdf_touch_last_x = -1;
  reader_state_.pdf_touch_last_y = -1;
  reader_state_.pdf_deferred_ready_at_ms = 0;

  // Initialize status log.
  status_log_file_ = nullptr;
  status_log_write_count_ = 0;
  LightLock_Init(&status_log_lock_); // Protects status log file access.

  // Initialize 3DS-specific state and hooks.
  pending_boot_reopen_ = false;
  skip_next_browser_present_ = false;
  is_new_3ds_ = false;
  is_homebrew_ = false;
  applet_suspended_ = false;
  applet_resume_pending_ = false;
  applet_suspend_handled_ = false;
  apt_hook_installed_ = false;
  shutdown_prepared_ = false;
  APT_CheckNew3DS(&is_new_3ds_);
  is_homebrew_ = envIsHomebrew();
  aptHook(&apt_hook_cookie_, App::AptHookCallback, this); // Install APT hook for handling app lifecycle events (suspend, resume, etc.).
  apt_hook_installed_ = true;

#ifdef DSLIBRIS_DEBUG
  // Debug builds should emit actionable logs by default.
  debug_log::SetLevel(DBG_LEVEL_DEBUG);
  debug_log::SetCategories(DBG_CAT_ALL);
#endif

  // Initialize UI components.
  ts = std::unique_ptr<Text>(new Text());
  ts->app = this;

  fontmenu = std::unique_ptr<FontMenu>(new FontMenu(this));
  bookmarkmenu = std::unique_ptr<BookmarkMenu>(new BookmarkMenu(this));
  chaptermenu = std::unique_ptr<ChapterMenu>(new ChapterMenu(this));

#ifdef DSLIBRIS_DEBUG
  // Log environment details for debugging purposes.
  DBG_LOGF(this, "ENV runtime=%s device=%s",
           is_homebrew_ ? "3dsx/homebrew" : "cia/title",
           is_new_3ds_ ? "new3ds" : "old3ds");
  if (debug_runtime::BackgroundWorkersDisabled())
  {
    DBG_LOG(this, "SAFE mode: background workers disabled");
  }
  if (debug_runtime::BrowserWarmupDisabled())
  {
    DBG_LOG(this, "BROWSER warmup: disabled");
  }
#endif
}

/* TODO: refactor app lifecycle management to better handle 3DS-specific events and states,
 such as app suspension, resumption, and homebrew vs CIA differences.
 This may involve more granular state tracking and event handling in the main loop and controllers.
 */
App::~App()
{
  PrepareForShutdown();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: aptUnhook begin");
#endif
  if (apt_hook_installed_)
  { // Clean up APT hook on app exit.
    aptUnhook(&apt_hook_cookie_);
    apt_hook_installed_ = false;
  }
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: start");
#endif
  LightLock_Lock(&status_log_lock_); // Ensure exclusive access to status log during cleanup.
  if (status_log_file_)
  {
    fflush(status_log_file_);
    fclose(status_log_file_);
    status_log_file_ = nullptr;
  }
  LightLock_Unlock(&status_log_lock_);
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: deleting books");
#endif
  // Delete all book instances to free resources.
  for (std::vector<Book *>::iterator it = books.begin(); it != books.end();
       it++)
    delete *it;
  books.clear();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: deleting buttons");
#endif
  // Delete all UI buttons.
  for (size_t i = 0; i < buttons.size(); i++)
    delete buttons[i];
  buttons.clear();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: done");
#endif
  UiButtonSkin_Exit(); // Clean up button skin resources.
}

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

void App::RunFontMenuFrame(u32 keys)
{
#ifdef DSLIBRIS_DEBUG
  static int s_font_frame_budget = 48;
  if (s_font_frame_budget > 0) // Log key state and dirty status for the font menu frame.
  {
    DBG_LOGF(this,
             "FONT frame keys=0x%08lx dirty=%d screen=%p right=%p left=%p ts_dirty=%d",
             (unsigned long)keys, fontmenu->isDirty() ? 1 : 0,
             (void *)ts->GetScreen(), (void *)ts->screenright,
             (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0);
    s_font_frame_budget--;
  }
#endif
  // Ensure first entry into font submenu is visible before any new key edge.
  if (fontmenu->isDirty())
  {
    ts->SetScreen(ts->screenright);
    fontmenu->draw();
    // Defensive: ensure framebuffer conversion sees this submenu redraw.
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_font_predraw_budget = 16;
    if (s_font_predraw_budget > 0)
    {
      DBG_LOGF(this,
               "FONT frame pre-draw done ts_dirty=%d screen=%p right=%p left=%p",
               ts->HasDirtyScreens() ? 1 : 0, (void *)ts->GetScreen(),
               (void *)ts->screenright, (void *)ts->screenleft);
      s_font_predraw_budget--;
    }
#endif
  }

  if (keys == 0)
    return;

  fontmenu->HandleInput(keys);
  if (fontmenu->isDirty())
  {
    ts->SetScreen(ts->screenright);
    fontmenu->draw();
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_font_draw_after_input_budget = 24;
    if (s_font_draw_after_input_budget > 0)
    {
      DBG_LOGF(this, "FONT frame draw-after-input ts_dirty=%d",
               ts->HasDirtyScreens() ? 1 : 0);
      s_font_draw_after_input_budget--;
    }
#endif
  }
}

void App::RunBookmarksMenuFrame(u32 keys)
{
  bookmarkmenu->HandleInput(keys);
  if (bookmarkmenu->IsDirty())
    bookmarkmenu->Draw();
}

void App::RunChaptersMenuFrame(u32 keys)
{
#ifdef DSLIBRIS_DEBUG
  static int s_chapters_frame_budget = 24;
  if (s_chapters_frame_budget > 0)
  {
    DBG_LOGF(this, "INDEX frame keys=0x%08lx dirty=%d", (unsigned long)keys,
             chaptermenu && chaptermenu->IsDirty() ? 1 : 0);
    s_chapters_frame_budget--;
  }
  static int s_chapters_input_budget = 64;
  if (s_chapters_input_budget > 0 && (keys != 0 || hidKeysHeld() != 0))
  {
    DBG_LOGF(this, "INDEX input down=0x%08lx held=0x%08lx",
             (unsigned long)keys, (unsigned long)hidKeysHeld());
    s_chapters_input_budget--;
  }
#endif
  // Draw first when invalidated so the index becomes visible even before any
  // new key edge arrives.
  if (chaptermenu->IsDirty())
  {
    chaptermenu->Draw();
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_chapters_predraw_budget = 16;
    if (s_chapters_predraw_budget > 0)
    {
      DBG_LOG(this, "INDEX frame pre-draw");
      s_chapters_predraw_budget--;
    }
#endif
  }

  // Chapters navigation is edge-triggered (`hidKeysDown` in main loop). Avoid
  // processing idle frames in the menu handler to keep this path deterministic.
  if (keys == 0)
    return;

  chaptermenu->HandleInput(keys);
  const bool dirty_after_input = chaptermenu && chaptermenu->IsDirty();
#ifdef DSLIBRIS_DEBUG
  {
    static int s_chapters_dirty_budget = 64;
    const bool dirty_before = chaptermenu->IsDirty();
    (void)dirty_before;
    if (s_chapters_dirty_budget > 0)
    {
      DBG_LOGF(this, "INDEX frame state dirty_after_input=%d", dirty_after_input ? 1 : 0);
      s_chapters_dirty_budget--;
    }
  }
#endif
  if (dirty_after_input)
    chaptermenu->Draw();
#ifdef DSLIBRIS_DEBUG
  static int s_chapters_draw_budget = 32;
  if (s_chapters_draw_budget > 0 && dirty_after_input)
  {
    DBG_LOG(this, "INDEX frame draw");
    s_chapters_draw_budget--;
  }
#endif
}

bool App::PresentIfDirty()
{
  if (applet_suspended_)
    return false;
  const bool had_dirty = ts->HasDirtyScreens();
  const bool wrote = ts->BlitToFramebuffer();
  const bool browser_idle_copy = (nav_.mode == AppMode::Browser && !had_dirty && wrote);
  if (browser_idle_copy)
    return false;
  if (wrote)
  {
    gfxFlushBuffers();
    gfxSwapBuffers();
    return true;
  }
  return false;
}

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

bool App::IsNew3dsDevice() const { return is_new_3ds_; }

bool App::IsHomebrewEnvironment() const { return is_homebrew_; }

bool App::IsAppletSuspended() const { return applet_suspended_; }

bool App::ShouldAbortWork() const
{
  return applet_suspended_ || nav_.mode == AppMode::Quit;
}

void App::PrepareForShutdown()
{
  if (shutdown_prepared_)
    return;
  shutdown_prepared_ = true;

#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "SHUTDOWN begin env=%s mode=%d current_session=%u opening_session=%u suspended=%u",
           is_homebrew_ ? "3dsx/homebrew" : "cia/title", (int)nav_.mode,
           reader_state_.current_book_session_id, reader_state_.opening.session_id,
           applet_suspended_ ? 1u : 0u);
#endif

  pending_boot_reopen_ = false;
  skip_next_browser_present_ = false;
  applet_resume_pending_ = false;
  applet_suspend_handled_ = false;
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();

  PersistPrefs();

#ifdef DSLIBRIS_DEBUG
  DBG_LOG(this, "SHUTDOWN cancel workers begin");
#endif
  const size_t removed_jobs = PauseBrowserJobs();
#ifndef DSLIBRIS_DEBUG
  (void)removed_jobs;
#endif

  Book *opening_book = reader_state_.opening.book;
  if (opening_book)
  {
    opening_book->RequestAbortOpen();
    opening_book->CancelFixedLayoutDeferredWork();
    opening_book->CancelAsyncReflowOpen();
  }

  CloseBook();
  nav_.mode = AppMode::Quit;

#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "SHUTDOWN cancel workers done removed_jobs=%u current_session=%u opening_session=%u",
           (unsigned)removed_jobs, reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
}

void App::AptHookCallback(APT_HookType hook, void *param)
{
  App *app = static_cast<App *>(param);
  if (app)
    app->HandleAppletHook(hook);
}

// Handle app lifecycle events from the APT hook, such as suspend, resume, and exit.
void App::HandleAppletHook(APT_HookType hook)
{
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "APPLET hook=%s mode=%d current_session=%u opening_session=%u",
           AppletHookName(hook), (int)nav_.mode,
           reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
  switch (hook)
  {
  case APTHOOK_ONSUSPEND:
    applet_suspended_ = true;
    applet_resume_pending_ = false;
    applet_suspend_handled_ = false;
    nav_.browser.wait_input_release = true;
    nav_.browser.last_interaction_ms = osGetTime();
    OnReaderAppletSuspendRequested();
    break;
  case APTHOOK_ONRESTORE:
  case APTHOOK_ONWAKEUP:
    applet_suspended_ = false;
    applet_resume_pending_ = true;
    break;
  case APTHOOK_ONEXIT:
    // Only set the quit flag here. PersistPrefs() writes to the SD card and
    // must NOT run inside an APT hook callback — doing so blocks the HOME Menu
    // from receiving the acknowledgment within its expected timing window,
    // which can cause the HOME Menu process to crash. Prefs are saved in
    // PrepareForShutdown() after aptMainLoop() returns.
    nav_.mode = AppMode::Quit;
    break;
  default:
    break;
  }
}

// Handle app suspension: pause ongoing work, mark browser state, and notify reader controller.
void App::HandleAppletSuspend()
{
  if (applet_suspend_handled_)
    return;
  applet_suspend_handled_ = true;
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  PauseBrowserJobs();
  OnReaderAppletSuspended();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "APPLET suspended mode=%d current_session=%u opening_session=%u",
           (int)nav_.mode,
           reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
}

// Handle app resumption: refresh browser state, mark views dirty, and notify reader controller.
void App::HandleAppletResume()
{
  if (!applet_resume_pending_)
    return;
  applet_resume_pending_ = false;
  applet_suspend_handled_ = false;
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  nav_.browser.view_dirty = true;
  nav_.prefs.view_dirty = true;
  if (ts)
    ts->MarkAllScreensDirty();
  RequestStatusRedraw();
  OnReaderAppletResumed();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "APPLET resumed mode=%d current_session=%u opening_session=%u",
           (int)nav_.mode, reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
}

// Main app run loop: execute startup sequence and then enter the main loop controller.
int App::Run(void)
{
  const int startup = startup_controller_->RunBootSequence();
  if (startup == 1)
    return 1;
  if (startup == 2)
    return 0;
  return main_loop_controller_->RunMainLoop();
}

// 3DS touch input — map physical touch to our logical buffer coordinates.
// The transform must be the inverse of Text::BlitToFramebuffer() for the
// currently active orientation.
touchPosition App::TouchRead()
{
  touchPosition raw;
  hidTouchRead(&raw); // Get raw touch coordinates from the 3DS hardware.
  touchPosition mapped;

  if (!orientation)
  {
    // Default "Turned Left" orientation (historical mapping), X un-mirrored.
    mapped.px = raw.py;       // -> sx
    mapped.py = 319 - raw.px; // -> sy
  }
  else
  {
    // "Turned Right" orientation (opposite page rotation), X un-mirrored.
    mapped.px = 239 - raw.py; // -> sx
    mapped.py = raw.px;       // -> sy
  }

  mapped.px = (u16)std::max(0, std::min(239, (int)mapped.px));
  mapped.py = (u16)std::max(0, std::min(319, (int)mapped.py));

#if ORIENTATION_DIAG
  if (g_orientation_touch_diag_budget > 0)
  {
    char dmsg[160];
    snprintf(dmsg, sizeof(dmsg),
             "ORIENT touch raw=(%u,%u) mapped=(%u,%u) turned_right=%d",
             (unsigned)raw.px, (unsigned)raw.py, (unsigned)mapped.px,
             (unsigned)mapped.py, orientation ? 1 : 0);
    DBG_LOG(this, dmsg);
    g_orientation_touch_diag_budget--;
  }
#endif

  return mapped;
}

static void DrawGradientToScreen(Text *ts, int colorMode, u16 *target_screen, int logical_h)
{
  if (!ts || !target_screen)
    return;
  const int w = ts->display.width;
  const int stride = ts->display.height;
  if (w <= 0 || stride <= 0 || logical_h <= 0)
    return;

  static std::vector<u16> gradient;
  static int cachedW = 0;
  static int cachedH = 0;
  static int cachedColorMode = -1;

  const ThemePalette &palette = GetThemePalette(colorMode);

  if (gradient.empty() || cachedW != w || cachedH != logical_h || cachedColorMode != colorMode)
  {
    gradient.resize((size_t)w * (size_t)logical_h);
    cachedW = w;
    cachedH = logical_h;
    cachedColorMode = colorMode;
    static const u8 kBayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };

    for (int y = 0; y < logical_h; y++)
    {
      const float tY = (logical_h > 1) ? ((float)y / (float)(logical_h - 1)) : 0.0f;
      for (int x = 0; x < w; x++)
      {
        const float dx =
            (w > 1)
                ? (((float)x - (float)(w - 1) * 0.5f) / ((float)(w - 1) * 0.5f))
                : 0.0f;
        const float edge = fabsf(dx);

        float r = palette.bgTopR + (palette.bgBotR - palette.bgTopR) * tY;
        float g = palette.bgTopG + (palette.bgBotG - palette.bgTopG) * tY;
        float b = palette.bgTopB + (palette.bgBotB - palette.bgTopB) * tY;

        const float vignette = 1.0f - 0.12f * powf(edge, 1.8f);

        const float bayer = (((float)kBayer4x4[y & 3][x & 3] + 0.5f) / 16.0f) -
                            0.5f;

        const u32 h0 = (u32)x * 73856093u;
        const u32 h1 = (u32)y * 19349663u;
        const u32 h2 = (h0 ^ h1 ^ 0x9E3779B9u);
        const float noise = ((((h2 >> 8) & 0xFF) / 255.0f) - 0.5f) * 0.6f;

        r = r * vignette + bayer * 3.8f + noise;
        g = g * vignette + bayer * 1.9f + noise * 0.6f;
        b = b * vignette + bayer * 3.8f + noise;

        gradient[(size_t)y * (size_t)w + (size_t)x] = RGB565FromU8(r, g, b);
      }
    }
  }

  for (int y = 0; y < logical_h; y++)
  {
    u16 *dst = target_screen + (size_t)y * (size_t)stride;
    const u16 *src = gradient.data() + (size_t)y * (size_t)w;
    memcpy(dst, src, (size_t)w * sizeof(u16));
  }
}

void App::DrawBottomGradientBackground()
{
  DrawGradientToScreen(ts.get(), colorMode, ts->screenright, 320);
}

void App::DrawTopGradientBackground()
{
  DrawGradientToScreen(ts.get(), colorMode, ts->screenleft, 400);
}

// Show the font selection menu, initializing it with the specified font mode (regular, bold, italic, etc.).
void App::ShowFontView(AppMode app_font_mode)
{
  nav_.mode = AppMode::PrefsFont;
  ts->SetScreen(ts->screenright);
  ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "FONT show mode=%d screen=%p right=%p left=%p ts_dirty=%d req=%d",
           (int)nav_.mode, (void *)ts->GetScreen(), (void *)ts->screenright,
           (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0,
           (int)app_font_mode);
#endif
  fontmenu->Open(app_font_mode);
}

// Show the library view (book browser), resetting shared bottom buttons and marking the view dirty for redraw.
void App::ShowLibraryView()
{
  // Reset shared bottom buttons immediately; prefs view reuses/moves them.
  buttonprev.Move(2, 296);
  buttonprev.Resize(66, 22);
  buttonprev.Label("prev");
  buttonnext.Move(172, 296);
  buttonnext.Resize(66, 22);
  buttonnext.Label("next");
  buttonprefs.Move(72, 296);
  buttonprefs.Resize(96, 22);
  buttonprefs.Label("settings");

  Book *bookcurrent_ = GetCurrentBook();
  if (bookcurrent_) {
    bookcurrent_->FlushPendingCacheSaves();
    // Cancel any in-progress PDF/CBZ strip render so the worker thread is
    // idle before browser warmup jobs can start touching MuPDF lock state.
    bookcurrent_->CancelFixedLayoutDeferredWork();
  }

  ResetBrowserMarquee();
  nav_.mode = AppMode::Browser;
  ts->SetScreen(ts->screenright);
  ts->MarkAllScreensDirty();
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms =
      osGetTime() + kBrowserReturnWarmupCooldownMs;
  nav_.browser.view_dirty = true;
  skip_next_browser_present_ = true;
  nav_.prefs.layout_notice_pending = false;
}

void App::ShowSettingsView(bool from_book)
{
  settings_controller_->ShowSettingsView(from_book);
}

void App::MarkBookLayoutDirty()
{
  // Bump the global layout generation so already-paginated books are reopened
  // before they are reused.
  reader_state_.layout_revision++;
  if (reader_state_.layout_revision == 0)
    reader_state_.layout_revision = 1;
  nav_.prefs.view_dirty = true;
  if (nav_.prefs.from_book && reader_state_.bookcurrent && reader_state_.bookcurrent->GetPageCount() > 0)
    nav_.prefs.layout_notice_pending = true;
}

bool App::BookNeedsRelayout(Book *book) const
{
  if (!book || !book->UsesTextLayoutSettings())
    return false;
  return book && app_flow_utils::NeedsBookRelayout(
                     book->GetPageCount(), book->GetLayoutRevision(),
                     reader_state_.layout_revision, book->NeedsMobiRenderRefresh());
}

void App::ShowBookmarksView()
{
  nav_.mode = AppMode::Bookmarks;
  ts->SetScreen(ts->screenright);
  bookmarkmenu->Init();
}

// Show the chapters/index view, deciding whether to open it based on the current book's format, TOC quality, and chapter availability. If chapters are unavailable, show a status message and redirect to settings.
void App::ShowChaptersView()
{
  DBG_LOG(this, "INDEX show begin");
  Book *book = reader_state_.bookcurrent;
  format_t format = FORMAT_UNDEF;
  bool toc_quality_known = false;
  if (book)
  {
    format = book->format;
    toc_quality_known = book->GetTocQuality() != TOC_QUALITY_UNKNOWN;
    DBG_LOGF(this,
             "INDEX request mode=%d book=%p fmt=%d chapters=%u tocq=%d tried=%d",
             (int)nav_.mode, (void *)book, (int)format,
             (unsigned)book->GetChapters().size(), (int)book->GetTocQuality(),
             book->tocResolveTried ? 1 : 0);
  }
  else
  {
    DBG_LOGF(this, "INDEX request mode=%d book=null", (int)nav_.mode);
  }
  app_flow_utils::ChaptersViewDecision decision =
      app_flow_utils::DecideChaptersView(
          book != nullptr, format, toc_quality_known,
          book ? book->tocResolveTried : false,
          book ? book->GetChapters().size() : 0);
  DBG_LOGF(this, "INDEX decision open=%d queue=%d reason=%d",
           decision.open_chapters ? 1 : 0, decision.queue_toc_resolve ? 1 : 0,
           (int)decision.reason);
  // Opening index must stay responsive. If chapters already exist, skip
  // deferred TOC resolve here and use the available chapter list.
  if (decision.queue_toc_resolve && book &&
      book->GetChapters().empty())
    QueueTocResolve(book);
  if (!decision.open_chapters)
  {
    if (decision.reason == app_flow_utils::ChaptersViewReason::NoCurrentBook)
    {
      PrintStatus("Index unavailable: no selected book");
    }
    else
    {
      PrintStatus("Index unavailable: no chapters");
    }
    ShowSettingsView(true);
    return;
  }
  if (!book || book->GetChapters().empty())
  {
    PrintStatus("Index unavailable: no chapters");
    ShowSettingsView(true);
    return;
  }
  nav_.mode = AppMode::Chapters;
  ts->SetScreen(ts->screenright);
  ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "INDEX show screen=%p right=%p left=%p ts_dirty=%d",
           (void *)ts->GetScreen(), (void *)ts->screenright,
           (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0);
#endif
  DBG_LOG(this, "INDEX show init menu begin");
  chaptermenu->Init();
  chaptermenu->DisableInitialReleaseWait();
  DBG_LOG(this, "INDEX show init menu end");
  DBG_LOGF(this, "INDEX open chapters=%u page_count=%u",
           (unsigned)book->GetChapters().size(), (unsigned)book->GetPageCount());
}

// Show the current book view (reader), ensuring a book is selected and marking the screen dirty for redraw.
void App::ShowCurrentBookView()
{
  if (!reader_state_.bookcurrent)
    return;
  nav_.mode = AppMode::Book;
  ts->SetScreen(ts->screenright);
  ts->MarkAllScreensDirty();
  RequestStatusRedraw();
}

void App::RequestStatusRedraw() { status_controller_->RequestStatusRedraw(); }

void App::UpdateStatus() { status_controller_->UpdateStatus(); }

// Set the screen orientation (turned right or left) and update touch input mapping, button layout, and mark screens dirty for redraw.
void App::SetOrientation(bool turned_right)
{
  // Keep software render orientation in sync with the current handedness.
  // TODO: rotating the whole console should also rotate/remap the physical
  // D-pad semantics for reader controls, not only the shoulder buttons.
  orientation = turned_right;
  if (ts)
  {
    ts->SetOrientation(turned_right);
    ts->MarkAllScreensDirty();
  }
  RequestStatusRedraw();
  nav_.browser.view_dirty = true;
  nav_.prefs.view_dirty = true;

  key.up = KEY_CPAD_UP;
  key.down = KEY_CPAD_DOWN;
  key.left = KEY_CPAD_LEFT;
  key.right = KEY_CPAD_RIGHT;
  key.dup = KEY_DUP;
  key.ddown = KEY_DDOWN;
  key.dleft = KEY_DLEFT;
  key.dright = KEY_DRIGHT;
  key.l = turned_right ? KEY_R : KEY_L;
  key.r = turned_right ? KEY_L : KEY_R;
  key.zl = turned_right ? KEY_ZR : KEY_ZL;
  key.zr = turned_right ? KEY_ZL : KEY_ZR;
  key.downrepeat = key.down | key.ddown;

#if ORIENTATION_DIAG
  g_orientation_touch_diag_budget = 2;
  DBG_LOGF(this, "ORIENT set turned_right=%d", turned_right ? 1 : 0);
#endif
}

// Initialize the top and bottom screens with double buffering and the correct pixel format, then clear the software buffers for both screens.
void App::InitScreens()
{
  // consoleInit() set the bottom screen to single-buffered and may have
  // changed the pixel format.  Take full control back before the main loop.
  gfxSetDoubleBuffering(GFX_TOP, true);
  gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
  gfxSetDoubleBuffering(GFX_BOTTOM, true);
  gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);

  // Clear our software buffers.
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
}

void App::PrintStatus(const char *msg)
{
  if (!msg)
    return;

  LightLock_Lock(&status_log_lock_);

  if (!status_log_file_)
  {
    status_log_file_ = fopen(paths::GetLogFile().c_str(), "a");
    if (status_log_file_)
      setvbuf(status_log_file_, NULL, _IOFBF, 4096);
  }

  if (status_log_file_)
  {
    time_t rawtime;
    struct tm *info;
    char buffer[80];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", info);

    fprintf(status_log_file_, "[%s] %s\n", buffer, msg);
    status_log_write_count_++;
#ifdef DSLIBRIS_DEBUG
    fflush(status_log_file_);
#else
    if ((status_log_write_count_ & 15u) == 0u)
      fflush(status_log_file_);
#endif
  }

  LightLock_Unlock(&status_log_lock_);
}

void App::PrintStatus(std::string msg) { PrintStatus(msg.c_str()); }
