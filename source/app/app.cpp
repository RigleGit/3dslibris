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
#include "book/book_renderer.h"
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
#include "shared/debug_log.h"
#include "shared/path_constants.h"
#include "parse.h"
#include "shared/debug_runtime_mode.h"
#include "settings/prefs.h"
#include "reader/book_switch_utils.h"
#include "ui/text.h"
#include "shared/screen_dimensions.h"
#include "ui/screen_layout_constants.h"

#ifndef ORIENTATION_DIAG
#define ORIENTATION_DIAG 0
#endif

namespace
{
  static const u64 kBrowserReturnWarmupCooldownMs = 1200;
} // end anonymous namespace
#include "ui/gradient_utils.h"

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

  bool new3ds = false;
  APT_CheckNew3DS(&new3ds);
  lifecycle_state_.SetNew3DS(new3ds);
  lifecycle_state_.SetHomebrew(envIsHomebrew());
  lifecycle_state_.InstallHook(App::AptHookCallback, this);


#ifdef DSLIBRIS_DEBUG
  // Debug builds should emit actionable logs by default.
  debug_log::SetLevel(DBG_LEVEL_DEBUG);
  debug_log::SetCategories(DBG_CAT_ALL);
#endif

  // Initialize UI components.
  ts = std::unique_ptr<Text>(new Text());
  ts->SetReporter(this);
  ts->SetFontDir(fontdir);

  fontmenu = std::unique_ptr<FontMenu>(new FontMenu(this));
  bookmarkmenu = std::unique_ptr<BookmarkMenu>(new BookmarkMenu(this));
  chaptermenu = std::unique_ptr<ChapterMenu>(new ChapterMenu(this));

#ifdef DSLIBRIS_DEBUG
  // Log environment details for debugging purposes.
  DBG_LOGF(this, "ENV runtime=%s device=%s",
           lifecycle_state_.IsHomebrew() ? "3dsx/homebrew" : "cia/title",
           lifecycle_state_.IsNew3DS() ? "new3ds" : "old3ds");
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
  lifecycle_state_.UninstallHook();
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

// NOTE: RunFontMenuFrame / RunBookmarksMenuFrame / RunChaptersMenuFrame
// moved to app_menu_frames.cpp.

bool App::PresentIfDirty()
{
  if (lifecycle_state_.IsSuspended())
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

// NOTE: trivial accessors and small forwarders moved to app_accessors.cpp.

// NOTE: applet lifecycle methods (PrepareForShutdown, AptHookCallback,
// HandleAppletHook, HandleAppletSuspend, HandleAppletResume) moved to
// app_lifecycle.cpp.

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

void App::DrawBottomGradientBackground()
{
  gradient_utils::DrawToScreen(ts.get(), colorMode, ts->screenright, screen_dims::kBottomScreenHeightPx);
}

void App::DrawTopGradientBackground()
{
  gradient_utils::DrawToScreen(ts.get(), colorMode, ts->screenleft, screen_dims::kTopScreenHeightPx);
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
  buttonprev.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
  buttonprev.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonprev.Label("prev");
  buttonprev.SetIcon(UI_BUTTON_ICON_PREV);
  buttonback.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
  buttonback.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonback.Label("back");
  buttonback.SetIcon(UI_BUTTON_ICON_BACK);
  buttonnext.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
  buttonnext.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonnext.Label("next");
  buttonnext.SetIcon(UI_BUTTON_ICON_NEXT);
  buttonprefs.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
  buttonprefs.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
  buttonprefs.Label("settings");
  buttonprefs.SetIcon(UI_BUTTON_ICON_GEAR);

  Book *bookcurrent_ = GetCurrentBook();
  if (bookcurrent_) {
    bookcurrent_->FlushPendingCacheSaves();
    // Cancel any in-progress PDF/CBZ strip render so the worker thread is
    // idle before browser warmup jobs can start touching MuPDF lock state.
    book_renderer::CancelFixedLayoutDeferredWork(bookcurrent_);
  }

  ResetBrowserMarquee();
  ResetPageRepeat();
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

void App::ReturnFromPrefs()
{
  if (IsBookSettingsContext() && GetCurrentBook()) {
    Book *book = GetCurrentBook();
    if (BookNeedsRelayout(book)) {
      SetSelectedBook(book);
      OpenBook();
    } else {
      ShowCurrentBookView();
      book_renderer::DrawCurrentView(book, ts.get());
      RequestStatusRedraw();
    }
  } else {
    ShowLibraryView();
  }
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
