/*
    dslibris-3ds - app_prefs.cpp
    Adapted from dslibris for Nintendo 3DS.
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

void App::PrefsInit() {
  const std::vector<std::string> labels{
      "regular font",       "bold font",    "italic font",
      "bold italic font",   "font size",    "paragraph spacing",
      "screen orientation", "clock format", "color mode",
      "bookmarks"};

  for (int i = 0; i < PREFS_BUTTON_COUNT; i++) {
    prefsButtons[i].Init(ts);
    prefsButtons[i].SetStyle(BUTTON_STYLE_SETTING);
    prefsButtons[i].Resize(230, 28);
    prefsButtons[i].SetLabel1(labels[i]);
    PrefsRefreshButton(i);
    prefsButtons[i].Move(5, i * 30);
  }

  prefsSelected = PREFS_BUTTON_FONTSIZE;
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

  for (int i = 0; i < PREFS_BUTTON_COUNT; i++)
    prefsButtons[i].Draw(ts->screenright, i == prefsSelected);

  // Draw library button below settings list (without overlapping list rows).
  buttonprefs.Move(162, 300);
  buttonprefs.Resize(70, 18);
  buttonprefs.Draw(ts->screenright);

  // Draw controls guide on the other screen
  ts->SetScreen(ts->screenleft);
  ts->SetColorMode(0); // Normal for controls guide
  ts->ClearScreen();
  ts->SetPen(ts->margin.left, 24);
  int tmpSize = ts->pixelsize;
  ts->SetPixelSize(12);
  ts->PrintString(
      "dslibris-3ds\n------------\n\n"
      "Controls:\n"
      "A / B / L / R : Turn Pages\n"
      "D-Pad Left/Right : Jump to Bookmarks\n"
      "START : Return to Library\n"
      "SELECT : Settings\n"
      "Y : Toggle Bookmark\n"
      "X : Invert Colors\n\n"
      "Settings are saved automatically.");
  ts->SetPixelSize(tmpSize);

  // restore state
  ts->SetStyle(style);
  ts->SetColorMode(colorMode);
  ts->margin.bottom = savedBottomMargin;
  ts->SetScreen(screen);

  prefs_view_dirty = false;
}

void App::PrefsHandleEvent() {
  u32 keys = hidKeysDown();

  if (keys & KEY_A) {
    PrefsHandlePress();
  } else if (keys & (KEY_SELECT | KEY_START | KEY_B | KEY_Y)) {
    ShowLibraryView();
  } else if (keys & (key.left | key.l)) {
    if (prefsSelected < PREFS_BUTTON_COUNT - 1) {
      prefsSelected++;
      prefs_view_dirty = true;
    }
  } else if (keys & (key.right | key.r)) {
    if (prefsSelected > 0) {
      prefsSelected--;
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

  if (buttonprefs.EnclosesPoint(coord.px, coord.py)) {
    ShowLibraryView();
    return;
  }

  for (u8 i = 0; i < PREFS_BUTTON_COUNT; i++) {
    if (prefsButtons[i].EnclosesPoint(coord.px, coord.py)) {
      if (i != prefsSelected) {
        prefsSelected = i;
      }

      if (i == PREFS_BUTTON_FONTSIZE) {
        if (coord.py > 2 + 188 / 2) {
          PrefsIncreasePixelSize();
        } else {
          PrefsDecreasePixelSize();
        }
      } else if (i == PREFS_BUTTON_PARASPACING) {
        if (coord.py > 2 + 188 / 2) {
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
}

void App::PrefsRefreshButton(int index) {
  char msg[64];
  switch (index) {
  case PREFS_BUTTON_FONT:
    prefsButtons[PREFS_BUTTON_FONT].SetLabel2(
        ts->GetFontName(TEXT_STYLE_REGULAR));
  case PREFS_BUTTON_FONT_BOLD:
    prefsButtons[PREFS_BUTTON_FONT_BOLD].SetLabel2(
        ts->GetFontName(TEXT_STYLE_BOLD));
  case PREFS_BUTTON_FONT_BOLDITALIC:
    prefsButtons[PREFS_BUTTON_FONT_BOLDITALIC].SetLabel2(
        ts->GetFontName(TEXT_STYLE_BOLDITALIC));
    break;
  case PREFS_BUTTON_FONT_ITALIC:
    prefsButtons[PREFS_BUTTON_FONT_ITALIC].SetLabel2(
        ts->GetFontName(TEXT_STYLE_ITALIC));
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
  case PREFS_BUTTON_BOOKMARKS:
    prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
        bookcurrent ? std::string(">") : std::string("(open book first)"));
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

  if (prefsSelected == PREFS_BUTTON_BOOKMARKS) {
    if (bookcurrent) {
      ShowBookmarksView();
    }
    return;
  }

  if (prefsSelected == PREFS_BUTTON_FONT) {
    mode = APP_MODE_PREFS_FONT;
  } else if (prefsSelected == PREFS_BUTTON_FONT_BOLD) {
    mode = APP_MODE_PREFS_FONT_BOLD;
  } else if (prefsSelected == PREFS_BUTTON_FONT_ITALIC) {
    mode = APP_MODE_PREFS_FONT_ITALIC;
  } else if (prefsSelected == PREFS_BUTTON_FONT_BOLDITALIC) {
    mode = APP_MODE_PREFS_FONT_BOLDITALIC;
  }
  ShowFontView(mode);
}
