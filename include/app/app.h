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

#include <deque>
#include <list>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "expat.h"

#include "ui/button.h"
#include "main.h"
#include "parse.h"
#include "ui/text.h"

class Book;
class Prefs;

#define APP_BROWSER_BUTTON_COUNT 4
#define APP_URL "http://github.com/rhaleblian/dslibris"

enum class AppMode : u8 {
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
};

enum prefsbuttonindex {
  PREFS_BUTTON_FONT_CONFIG,
  PREFS_BUTTON_FONTSIZE,
  PREFS_BUTTON_PARASPACING,
  PREFS_BUTTON_ORIENTATION,
  PREFS_BUTTON_TIME24H,
  PREFS_BUTTON_COLORMODE,
  PREFS_BUTTON_INDEX,
  PREFS_BUTTON_BOOKMARKS,
  PREFS_BUTTON_COUNT
};

enum app_job_type_t {
  APP_JOB_INDEX_METADATA,
  APP_JOB_EXTRACT_COVER,
  APP_JOB_RESOLVE_TOC
};

struct app_job_t {
  app_job_type_t type;
  Book *book;
};

//! \brief Main application.
//!
//!	\detail Top-level singleton class that handles application
//! initialization,
//! interaction loop, drawing everything but text, and logging.

class App {
public:
  App();
  ~App();

  Text *ts;
  Prefs *prefs;        //! User-configurable settings.
  std::string fontdir; //! Directory to search for font files
  bool melonds;        //! Are we running in melonDS?

  //! key functions are remappable to support screen flipping.
  struct {
    u32 up, down, left, right, l, r, a, b, x, y, start, select;
    u32 downrepeat;
  } key;

  std::vector<Button *> buttons;
  Button buttonprev, buttonnext, buttonprefs; //! Buttons on browser bottom.
  std::string bookdir; //! Search here for XHTML.
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

  class FontMenu *fontmenu; //! Font selection menu.
  class BookmarkMenu *bookmarkmenu;
  class ChapterMenu *chaptermenu;

  // app.cpp
  void PrintStatus(const char *msg);
  void PrintStatus(std::string msg);
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
  void MarkBrowserDirty();
  void MarkPrefsDirty();
  bool IsPrefsDirty() const;
  bool IsBrowserDirty() const;

  // app_book.cpp
  void CloseBook();
  int GetBookIndex(Book *);
  void HandleEventInBook();
  u8 OpenBook();
  void ToggleBookmark();
  void MarkBookLayoutDirty();
  void ShowCurrentBookView();

  void PrefsRefreshButton(int index);
  void PrefsRefreshButtonFont();
  void PrefsRefreshButtonFontBold();
  void PrefsRefreshButtonFontItalic();
  void PrefsRefreshButtonFontBoldItalic();
  void ShowSettingsView(bool from_book = false);
  inline bool IsBookSettingsContext() const { return prefs_view_.from_book; }
  void DrawBottomGradientBackground();

private:
  struct BrowserState {
    Book *selected_book;
    int page_start;
    bool view_dirty;
    bool wait_input_release;
  };

  struct PrefsViewState {
    int selected_index;
    bool view_dirty;
    bool from_book;
    bool layout_notice_pending;
  };

  struct StatusState {
    int last_minute;
    int last_percent_tenths;
    Book *progress_lock_book;
    int progress_pagecount_lock;
    bool force_redraw;
  };

  static bool IsFontMode(AppMode mode);

  AppMode mode_;
  BrowserState browser_;
  PrefsViewState prefs_view_;
  StatusState status_;
  Book *bookcurrent_;
  std::deque<app_job_t> job_queue;
  unsigned int layout_revision;
  bool pdf_touch_drag_active_;
  int pdf_touch_last_x_;
  int pdf_touch_last_y_;
  u64 pdf_deferred_ready_at_ms_;

  int FindBooks();
  void InitScreens();
  void SetOrientation(bool flipped);
  void ShowFontView(AppMode app_mode);
  void ShowLibraryView();
  void ShowBookmarksView();
  void ShowChaptersView();

  // app_Browser.cpp
  void browser_draw();
  void browser_handleevent();
  void browser_init();
  void browser_nextpage();
  void browser_prevpage();
  bool HasQueuedJob(app_job_type_t type, Book *book) const;
  void EnqueueJob(app_job_type_t type, Book *book);
  void QueueBookWarmup(Book *book);
  void QueueTocResolve(Book *book);
  void ProcessJobs(u32 budget_ms);

  // app_prefs.cpp
  void PrefsDraw();
  void PrefsHandleEvent();
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
  bool BookNeedsRelayout(Book *book) const;
};
