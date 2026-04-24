/*
 Copyright (C) 2007-2009 Ray Haleblian (ray23@sourceforge.net)

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

 To contact the copyright holder: rayh23@sourceforge.net
 */

/*
  3DS port modifications by Rigle (summary):
  - Added 3DS runtime/application state, menu modes, and screen orchestration.
  - Integrated browser/settings/reader flows with touch + key input mapping.
  - Added cover/index preload queue, TOC trust/fallback logic, and telemetry.
*/

#pragma once
/*!
\mainpage

dslibris is an ebook reader for the Nintendo DS family
of handheld game consoles.

For information about prerequisites, building, and installing,
see

https://github.com/rhaleblian/dslibris/blob/master/README.md

This documentation was built by running

  make doc

in the repo root directory. The origin Git repo is located at

https://github.com/rhaleblian/dslibris

\author ray haleblian

*/

#include <list>
#include <memory>
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#include "expat.h"

#include "menus/bookmark_menu.h"
#include "menus/chapter_menu.h"
#include "settings/font.h"
#include "ui/button.h"
#include "main.h"
#include "parse.h"
#include "settings/prefs_button_ids.h"
#include "ui/text.h"
#include "shared/status_reporter.h"

class Book;
class Prefs;
class LibraryController;
class ReaderController;
class SettingsController;
class StatusController;
class StartupController;
class MainLoopController;

#define APP_BROWSER_BUTTON_COUNT 4

// Forward declarations for controller classes to avoid circular dependencies in headers.
enum class AppMode : u8
{
  Book = 0,
  Browser = 1,
  Prefs = 2,
  PrefsFont = 3,
  PrefsFontBold = 4,
  PrefsFontItalic = 5,
  PrefsFontBoldItalic = 6,
  Quit = 7,
  Bookmarks = 8,
  Chapters = 9,
  Opening = 10,
};

enum app_job_type_t
{
  APP_JOB_INDEX_METADATA,
  APP_JOB_EXTRACT_COVER,
  APP_JOB_RESOLVE_TOC
};

struct app_job_t
{
  app_job_type_t type;
  Book *book;
};

//! \brief Main application.
//!
//!	\detail Top-level singleton class that handles application
//! initialization,
//! interaction loop, drawing everything but text, and logging.

class App : public IStatusReporter
{
public:
  static App *GetInstance();
  static void SetInstance(App *instance);

  App();
  ~App();

  std::unique_ptr<Text> ts;
  std::unique_ptr<Prefs> prefs; //! User-configurable settings.
  std::string fontdir;          //! Directory to search for font files

  //! key functions are remappable to support screen flipping.
  struct
  {
    u32 up, down, left, right, // Circle Pad directions.
         zl, zr, l, r,              // Shoulder buttons.
         dup, ddown, dleft, dright, // D-pad directions as separate entries for remapping buttons.
         a, b, x, y,          // Face buttons.
         start, select;       // Start and Select buttons.
    u32 downrepeat;
  } key;

  std::vector<Button *> buttons;
  Button buttonprev, buttonnext, buttonprefs; //! Buttons on browser bottom.
  std::string bookdir;                        //! Search here for XHTML.
  std::vector<Book *> books;
  //! reopen book from last session on startup?
  bool reopen;
  //! Write baked text to cache?
  bool cache;
  //! user data block passed to expat callbacks.
  parsedata_t parsedata;
  u8 orientation;
  u8 colorMode;
  u8 paraspacing, paraindent;

  Button prefsButtons[PREFS_BUTTON_COUNT];

  std::unique_ptr<FontMenu> fontmenu; //! Font selection menu.
  std::unique_ptr<BookmarkMenu> bookmarkmenu;
  std::unique_ptr<ChapterMenu> chaptermenu;

  // app.cpp
  void PrintStatus(const char *msg) override;
  void PrintStatus(std::string msg) override;
  int Run(void);
  touchPosition TouchRead();
  void UpdateStatus();
  void RequestStatusRedraw();
  void parse_error(XML_ParserStruct *ps);
  AppMode GetMode() const;
  void SetMode(AppMode mode);
  Book *GetSelectedBook() const;
  void SetSelectedBook(Book *book);
  Book *GetCurrentBook() const;
  void SetCurrentBook(Book *book);
  int BookCount() const;
  int GetSelectedBookIndex() const;
  int GetBrowserPageStart() const;
  void SetBrowserPageStart(int page_start);
  u64 GetBrowserLastInteractionMs() const;
  void SetBrowserLastInteractionMs(u64 ms);
  bool IsBrowserWaitingInputRelease() const;
  void SetBrowserWaitingInputRelease(bool wait_input_release);
  void SetBrowserDirty(bool dirty);
  void MarkBrowserDirty();
  int GetPrefsSelectedIndex() const;
  void SetPrefsSelectedIndex(int selected_index);
  void SetBookSettingsContext(bool from_book);
  bool IsPrefsLayoutNoticePending() const;
  void SetPrefsLayoutNoticePending(bool pending);
  void SetPrefsDirty(bool dirty);
  void MarkPrefsDirty();
  bool IsPrefsDirty() const;
  bool IsBrowserDirty() const;
  bool ShouldSkipNextBrowserPresent() const;
  void ClearSkipNextBrowserPresent();
  void ProcessJobs(u32 budget_ms);
  void browser_draw();
  void browser_handleevent();
  void browser_init();
  void TickBrowserWarmup();
  void browser_tick_marquee();
  void ResetBrowserMarquee();
  void PrefsDraw();
  void PrefsHandleEvent();
  void PersistPrefs();
  void RunFontMenuFrame(u32 keys);
  void RunBookmarksMenuFrame(u32 keys);
  void RunChaptersMenuFrame(u32 keys);
  bool PresentIfDirty();
  int StartupFindBooks();
  void StartupPrepareLibrary();
  void StartupInitUiAndBrowser();
  void StartupInitScreens();
  bool HasPendingBootReopen() const;
  void SetPendingBootReopen(bool pending);

  // app_book.cpp
  void CloseBook();
  int GetBookIndex(Book *);
  void HandleEventInBook();
  void HandleEventInOpening();
  u8 OpenBook();
  void ToggleBookmark();
  void MarkBookLayoutDirty();
  void ShowCurrentBookView();
  bool IsOpeningPending() const;
  void SetOpeningPending(bool pending);
  Book *GetOpeningBook() const;
  void SetOpeningBook(Book *book);
  unsigned int GetOpeningSessionId() const;
  void SetOpeningSessionId(unsigned int session_id);
  bool IsOpeningNeedsRelayout() const;
  void SetOpeningNeedsRelayout(bool needs_relayout);
  int GetOpeningOldPageCount() const;
  void SetOpeningOldPageCount(int old_page_count);
  int GetOpeningOldPosition() const;
  void SetOpeningOldPosition(int old_position);
  unsigned int GetOpeningSpineDone() const;
  unsigned int GetOpeningSpineTotal() const;
  void SetOpeningSpineProgress(unsigned int done, unsigned int total);
  unsigned int GetOpeningProgressSeq() const;
  unsigned int GetOpeningDrawnProgressSeq() const;
  void SetOpeningDrawnProgressSeq(unsigned int seq);
  std::list<int> &MutableOpeningOldBookmarks();
  u64 GetOpeningStartedAtMs() const;
  void SetOpeningStartedAtMs(u64 started_at_ms);
  unsigned int GetCurrentBookSessionId() const;
  void SetCurrentBookSessionId(unsigned int session_id);
  unsigned int AllocateBookSessionId();
  bool IsDeferredRelayoutPending() const;
  void SetDeferredRelayoutPending(bool pending);
  Book *GetDeferredRelayoutBook() const;
  void SetDeferredRelayoutBook(Book *book);
  int GetDeferredRelayoutOldPageCount() const;
  void SetDeferredRelayoutOldPageCount(int old_page_count);
  int GetDeferredRelayoutOldPosition() const;
  void SetDeferredRelayoutOldPosition(int old_position);
  std::list<int> &MutableDeferredRelayoutOldBookmarks();
  int GetDeferredRelayoutInitialPosition() const;
  void SetDeferredRelayoutInitialPosition(int initial_position);
  unsigned int GetLayoutRevision() const;
  void SetLayoutRevision(unsigned int layout_revision);
  bool IsPdfTouchDragActive() const;
  void SetPdfTouchDragActive(bool active);
  int GetPdfTouchLastX() const;
  void SetPdfTouchLastX(int x);
  int GetPdfTouchLastY() const;
  void SetPdfTouchLastY(int y);
  u64 GetPdfDeferredReadyAtMs() const;
  void SetPdfDeferredReadyAtMs(u64 ready_at_ms);
  bool IsNew3dsDevice() const;
  bool IsHomebrewEnvironment() const;
  bool IsAppletSuspended() const;
  bool ShouldAbortWork() const override;
  void PrepareForShutdown();
  void HandleAppletSuspend();
  void HandleAppletResume();

  void PrefsRefreshButton(int index);
  void PrefsRefreshButtonFont();
  void PrefsRefreshButtonFontBold();
  void PrefsRefreshButtonFontItalic();
  void PrefsRefreshButtonFontBoldItalic();
  void ShowSettingsView(bool from_book = false);
  inline bool IsBookSettingsContext() const { return nav_.prefs.from_book; }
  void DrawBottomGradientBackground();
  void DrawTopGradientBackground();
  void SetOrientation(bool flipped);
  void ShowFontView(AppMode app_mode);
  void ShowLibraryView();
  void ShowBookmarksView();
  void ShowChaptersView();
  bool BookNeedsRelayout(Book *book) const;
  size_t PauseBrowserJobs();
  void LoadVisibleBrowserCoverCaches();

private:
  static App *s_instance_;
  struct BrowserState
  {
    Book *selected_book;
    int page_start;
    bool view_dirty;
    bool wait_input_release;
    u64 last_interaction_ms;
  };

  struct PrefsViewState
  {
    int selected_index;
    bool view_dirty;
    bool from_book;
    bool layout_notice_pending;
  };

  struct OpeningState
  {
    bool pending;
    Book *book;
    unsigned int session_id;
    bool needs_relayout;
    int old_page_count;
    int old_position;
    std::list<int> old_bookmarks;
    u64 started_at_ms;
    unsigned int spine_done;
    unsigned int spine_total;
    unsigned int progress_seq;
    unsigned int drawn_progress_seq;

    OpeningState()
        : pending(false), book(nullptr), session_id(0), needs_relayout(false),
          old_page_count(0), old_position(0), old_bookmarks(),
          started_at_ms(0), spine_done(0), spine_total(0), progress_seq(0),
          drawn_progress_seq(0) {}
  };

  struct DeferredRelayoutState
  {
    bool pending;
    Book *book;
    int old_page_count;
    int old_position;
    std::list<int> old_bookmarks;
    int initial_position;

    DeferredRelayoutState()
        : pending(false), book(nullptr), old_page_count(0), old_position(0),
          old_bookmarks(), initial_position(0) {}
  };

  static bool IsFontMode(AppMode mode);

  struct NavigationState
  {
    AppMode mode;
    BrowserState browser;
    PrefsViewState prefs;

    NavigationState()
        : mode(AppMode::Browser), browser(), prefs() {}
  };

  struct ReaderRuntimeState
  {
    OpeningState opening;
    DeferredRelayoutState deferred_relayout;
    Book *bookcurrent;
    unsigned int current_book_session_id;
    unsigned int next_book_session_id;
    unsigned int layout_revision;
    bool pdf_touch_drag_active;
    int pdf_touch_last_x;
    int pdf_touch_last_y;
    u64 pdf_deferred_ready_at_ms;

    ReaderRuntimeState()
        : opening(), deferred_relayout(), bookcurrent(nullptr),
          current_book_session_id(0), next_book_session_id(1),
          layout_revision(0),
          pdf_touch_drag_active(false), pdf_touch_last_x(-1),
          pdf_touch_last_y(-1), pdf_deferred_ready_at_ms(0) {}
  };

  NavigationState nav_;
  std::unique_ptr<LibraryController> library_controller_;
  std::unique_ptr<ReaderController> reader_controller_;
  std::unique_ptr<SettingsController> settings_controller_;
  std::unique_ptr<StatusController> status_controller_;
  std::unique_ptr<StartupController> startup_controller_;
  std::unique_ptr<MainLoopController> main_loop_controller_;
  ReaderRuntimeState reader_state_;
  FILE *status_log_file_;
  unsigned int status_log_write_count_;
  LightLock status_log_lock_;
  bool pending_boot_reopen_;
  bool skip_next_browser_present_;
  bool is_new_3ds_;
  bool is_homebrew_;
  bool applet_suspended_;
  bool applet_resume_pending_;
  bool applet_suspend_handled_;
  aptHookCookie apt_hook_cookie_;
  bool apt_hook_installed_;
  bool shutdown_prepared_;

  void InitScreens();
  static void AptHookCallback(APT_HookType hook, void *param);
  void HandleAppletHook(APT_HookType hook);
  void OnReaderAppletSuspendRequested();
  void OnReaderAppletSuspended();
  void OnReaderAppletResumed();

  // app_Browser.cpp
  void UnloadNonVisibleBrowserCoverCaches();
  void browser_nextpage();
  void browser_prevpage();
  void PrioritizeSelectedBookJobs(Book *selected_book);
  bool HasQueuedJob(app_job_type_t type, Book *book) const;
  void EnqueueJob(app_job_type_t type, Book *book);
  void QueueBookWarmup(Book *book);
  void QueueTocResolve(Book *book);

  // app_prefs.cpp
  void PrefsHandlePress();
  void PrefsHandleTouch();
  void PrefsInit();
  void PrefsIncreasePixelSize();
  void PrefsDecreasePixelSize();
  void PrefsIncreaseParaspacing();
  void PrefsDecreaseParaspacing();
  void PrefsFlipOrientation();
  void ToggleCurrentBookMobiLineWrapFix();
  u8 PrefsVisibleButtonCount() const;
};
