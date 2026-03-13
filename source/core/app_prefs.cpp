/*
    3dslibris - app_prefs.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Context-aware settings rows (library vs per-book actions).
    - 3DS touch handling for row controls and footer button overlays.
    - Dynamic index/bookmark availability and runtime UI refresh behavior.
*/

#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <3ds.h>

#include "book.h"
#include "button.h"
#include "main.h"
#include "parse.h"
#include "text.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

static const int PREFS_LIBRARY_BTN_X = 130;
static const int PREFS_LIBRARY_BTN_Y = 286;
static const int PREFS_LIBRARY_BTN_W = 104;
static const int PREFS_LIBRARY_BTN_H = 26;
static const int PREFS_ROW_X = 5;
static const int PREFS_ROW_W = 230;

static bool CanOpenBookIndexInCurrentContext(App *app) {
  if (!app || !app->IsBookSettingsContext() || !app->bookcurrent)
    return false;
  if (!app->bookcurrent->GetChapters().empty())
    return true;
  return app->bookcurrent->format == FORMAT_EPUB;
}

static bool CanOpenSelectedBookIndex(App *app) {
  if (!app || !app->bookselected)
    return false;
  if (!app->bookselected->GetChapters().empty())
    return true;
  return app->bookselected->format == FORMAT_EPUB;
}

u8 App::PrefsVisibleButtonCount() const {
  // General settings hide per-book actions like index/bookmarks.
  return prefs_book_context ? PREFS_BUTTON_COUNT : PREFS_BUTTON_INDEX;
}

void App::PrefsInit() {
  const std::vector<std::string> labels{
      "font configuration", "font size",    "paragraph spacing",
      "screen orientation", "clock format", "color mode",
      "index",
      "bookmarks"};

  for (int i = 0; i < PREFS_BUTTON_COUNT; i++) {
    prefsButtons[i].Init(ts);
    prefsButtons[i].SetStyle(BUTTON_STYLE_SETTING);
    prefsButtons[i].Resize(230, 36);
    prefsButtons[i].SetLabel1(labels[i]);
    PrefsRefreshButton(i);
    prefsButtons[i].Move(5, i * 38);
  }

  prefsSelected = PREFS_BUTTON_FONT_CONFIG;
}

void App::PrefsDraw() {
  // save state
  int colorMode = ts->GetColorMode();
  u16 *screen = ts->GetScreen();
  int style = ts->GetStyle();
  int savedBottomMargin = ts->margin.bottom;

  // Settings UI uses full-screen layout; page margins should not clip text.
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenright);
  ts->SetColorMode(0); // Normal for prefs menu
  ts->ClearScreen();
  DrawBottomGradientBackground();

  u8 visibleCount = PrefsVisibleButtonCount();
  if (visibleCount == 0)
    visibleCount = 1;
  if (prefsSelected >= visibleCount)
    prefsSelected = visibleCount - 1;

  // Dynamic rows depend on current context/book; refresh before drawing.
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);

  for (int i = 0; i < visibleCount; i++)
    prefsButtons[i].Draw(ts->screenright, i == prefsSelected);

  // Draw library button below settings list (without overlapping list rows).
  buttonprefs.Move(PREFS_LIBRARY_BTN_X, PREFS_LIBRARY_BTN_Y);
  buttonprefs.Resize(PREFS_LIBRARY_BTN_W, PREFS_LIBRARY_BTN_H);
  buttonprefs.Draw(ts->screenright);

  ts->PrintSplash(ts->screenleft);

  // restore state
  ts->SetStyle(style);
  ts->SetColorMode(colorMode);
  ts->margin.bottom = savedBottomMargin;
  ts->SetScreen(screen);

  prefs_view_dirty = false;
}

void App::PrefsHandleEvent() {
  u32 keys = hidKeysDown();
  u8 visibleCount = PrefsVisibleButtonCount();
  if (visibleCount == 0)
    visibleCount = 1;
  if (prefsSelected >= visibleCount)
    prefsSelected = visibleCount - 1;

  if (keys & KEY_A) {
    PrefsHandlePress();
  } else if (keys & (KEY_SELECT | KEY_START | KEY_B | KEY_Y)) {
    ShowLibraryView();
  } else if (keys & (key.left | key.l)) {
    if (prefsSelected > 0) {
      prefsSelected--;
      prefs_view_dirty = true;
    }
  } else if (keys & (key.right | key.r)) {
    if (prefsSelected < visibleCount - 1) {
      prefsSelected++;
      prefs_view_dirty = true;
    }
  } else if (prefsSelected == PREFS_BUTTON_FONTSIZE && (keys & key.up)) {
    PrefsDecreasePixelSize();
  } else if (prefsSelected == PREFS_BUTTON_FONTSIZE && (keys & key.down)) {
    PrefsIncreasePixelSize();
  } else if (prefsSelected == PREFS_BUTTON_PARASPACING && (keys & key.up)) {
    PrefsDecreaseParaspacing();
  } else if (prefsSelected == PREFS_BUTTON_PARASPACING && (keys & key.down)) {
    PrefsIncreaseParaspacing();
  } else if (keys & KEY_TOUCH) {
    PrefsHandleTouch();
  }
}

void App::PrefsHandleTouch() {
  touchPosition coord = TouchRead();
  const int footerX = (int)coord.px;
  const int footerY = (int)coord.py;

  // Keep touch hitbox synced with drawing geometry.
  buttonprefs.Move(PREFS_LIBRARY_BTN_X, PREFS_LIBRARY_BTN_Y);
  buttonprefs.Resize(PREFS_LIBRARY_BTN_W, PREFS_LIBRARY_BTN_H);
  auto enclosesWithSlack = [&](Button &button, int x, int y) {
    for (int dy = -8; dy <= 8; dy += 4) {
      for (int dx = -8; dx <= 8; dx += 4) {
        int tx = x + dx;
        int ty = y + dy;
        if (tx < 0 || ty < 0)
          continue;
        if (button.EnclosesPoint((u16)tx, (u16)ty))
          return true;
      }
    }
    return false;
  };

  // Strict priority for visible library button area.
  if (enclosesWithSlack(buttonprefs, footerX, footerY)) {
    ShowLibraryView();
    return;
  }

  u8 visibleCount = PrefsVisibleButtonCount();
  for (u8 i = 0; i < visibleCount; i++) {
    if (prefsButtons[i].EnclosesPoint(coord.px, coord.py)) {
      if (i != prefsSelected) {
        prefsSelected = i;
      }

      if (i == PREFS_BUTTON_FONTSIZE) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreasePixelSize();
        } else {
          PrefsDecreasePixelSize();
        }
      } else if (i == PREFS_BUTTON_PARASPACING) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreaseParaspacing();
        } else {
          PrefsDecreaseParaspacing();
        }
      } else if (i == PREFS_BUTTON_ORIENTATION) {
        PrefsFlipOrientation();
      } else if (i == PREFS_BUTTON_TIME24H) {
        prefs->time24h = !prefs->time24h;
        PrefsRefreshButton(PREFS_BUTTON_TIME24H);
        prefs->Write();
        prefs_view_dirty = true;
      } else if (i == PREFS_BUTTON_COLORMODE) {
        int mode = ts->GetColorMode();
        ts->SetColorMode((mode + 1) % 3);
        PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
        prefs->Write();
        prefs_view_dirty = true;
      } else {
        PrefsHandlePress();
      }

      break;
    }
  }

  if (prefs_view_dirty)
    PrefsDraw();
}

void App::PrefsIncreasePixelSize() {
  if (ts->pixelsize < 18) {
    ts->SetPixelSize(ts->pixelsize + 1);
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    prefs->Write();
  }
}

void App::PrefsDecreasePixelSize() {
  if (ts->pixelsize > 6) {
    ts->SetPixelSize(ts->pixelsize - 1);
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    prefs->Write();
  }
}

void App::PrefsIncreaseParaspacing() {
  if (paraspacing < 2) {
    paraspacing++;
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    prefs->Write();
  }
}

void App::PrefsDecreaseParaspacing() {
  if (paraspacing > 0) {
    paraspacing--;
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    prefs->Write();
  }
}

void App::PrefsFlipOrientation() {
  SetOrientation(!orientation);
  PrefsRefreshButton(PREFS_BUTTON_ORIENTATION);
  prefs->Write();
  // Keep settings view synchronized immediately after rotation toggle.
  if (mode == APP_MODE_PREFS)
    PrefsDraw();
}

void App::PrefsRefreshButton(int index) {
  char msg[64];
  switch (index) {
  case PREFS_BUTTON_FONT_CONFIG:
    prefsButtons[PREFS_BUTTON_FONT_CONFIG].SetLabel2(
        std::string("open menu >"));
    break;
  case PREFS_BUTTON_FONTSIZE:
    sprintf(msg, "                        < %d >  ", ts->GetPixelSize());
    prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string(msg));
    break;
  case PREFS_BUTTON_PARASPACING:
    sprintf(msg, "                         < %d >  ", paraspacing);
    prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(std::string(msg));
    break;
  case PREFS_BUTTON_ORIENTATION:
    prefsButtons[PREFS_BUTTON_ORIENTATION].SetLabel2(
        orientation ? std::string("Turned Right") : std::string("Turned Left"));
    break;
  case PREFS_BUTTON_TIME24H:
    prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
        prefs->time24h ? std::string("24h Format") : std::string("12h Format"));
    break;
  case PREFS_BUTTON_COLORMODE: {
    int mode = ts->GetColorMode();
    if (mode == 0)
      prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Normal"));
    else if (mode == 1)
      prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Dark"));
    else
      prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Sepia"));
    break;
  }
  case PREFS_BUTTON_INDEX:
    if (CanOpenBookIndexInCurrentContext(this)) {
      prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string(">"));
    } else if (CanOpenSelectedBookIndex(this)) {
      prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(
          std::string("(open selected book)"));
    } else {
      prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string("(not available)"));
    }
    break;
  case PREFS_BUTTON_BOOKMARKS:
    prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
        (prefs_book_context && bookcurrent) ? std::string(">")
                                            : std::string("(open selected book)"));
    break;
  }
  prefs_view_dirty = true;
}

void App::PrefsHandlePress() {
  //! Go to font view or flip orientation.

  if (prefsSelected == PREFS_BUTTON_ORIENTATION) {
    PrefsFlipOrientation();
    prefs_view_dirty = true;
    return;
  }

  if (prefsSelected == PREFS_BUTTON_TIME24H) {
    prefs->time24h = !prefs->time24h;
    PrefsRefreshButton(PREFS_BUTTON_TIME24H);
    prefs->Write();
    prefs_view_dirty = true;
    return;
  }

  if (prefsSelected == PREFS_BUTTON_COLORMODE) {
    int mode = ts->GetColorMode();
    ts->SetColorMode((mode + 1) % 3);
    PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
    prefs->Write();
    prefs_view_dirty = true;
    prefs_view_dirty = true;
    return;
  }

  if (prefsSelected == PREFS_BUTTON_INDEX) {
    if (CanOpenBookIndexInCurrentContext(this)) {
      ShowChaptersView();
    } else if (CanOpenSelectedBookIndex(this)) {
      OpenBook();
    } else {
      PrintStatus("Index unavailable for this book");
      PrefsRefreshButton(PREFS_BUTTON_INDEX);
      prefs_view_dirty = true;
    }
    return;
  }

  if (prefsSelected == PREFS_BUTTON_BOOKMARKS) {
    if (prefs_book_context && bookcurrent) {
      ShowBookmarksView();
    } else if (!prefs_book_context && bookselected) {
      OpenBook();
    }
    return;
  }

  if (prefsSelected == PREFS_BUTTON_FONT_CONFIG) {
    mode = APP_MODE_PREFS_FONT;
    ShowFontView(mode);
    return;
  }
}
