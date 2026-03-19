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

#include "app/app.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <3ds.h>

#include "book/book.h"
#include "ui/button.h"
#include "color_utils.h"
#include "main.h"
#include "parse.h"
#include "settings/prefs.h"
#include "ui/text.h"

static const int PREFS_LIBRARY_BTN_X = 130;
static const int PREFS_LIBRARY_BTN_Y = 286;
static const int PREFS_LIBRARY_BTN_W = 104;
static const int PREFS_LIBRARY_BTN_H = 26;
static const int PREFS_ROW_X = 5;
static const int PREFS_ROW_W = 230;

static u8 NormalizeVisibleCount(u8 count) { return count == 0 ? 1 : count; }

static void ClampSelectedIndex(int *selected, u8 visibleCount) {
  if (!selected || visibleCount == 0)
    return;
  if (*selected >= visibleCount)
    *selected = visibleCount - 1;
}

static void SyncLibraryButtonLayout(Button *button) {
  if (!button)
    return;
  button->Move(PREFS_LIBRARY_BTN_X, PREFS_LIBRARY_BTN_Y);
  button->Resize(PREFS_LIBRARY_BTN_W, PREFS_LIBRARY_BTN_H);
}

static void ToggleClockFormatSetting(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->time24h = !prefs->time24h;
  prefs->Write();
}

static void CycleColorMode(Text *ts) {
  if (!ts)
    return;
  int mode = ts->GetColorMode();
  ts->SetColorMode((mode + 1) % 3);
}

static bool CanOpenBookIndexInCurrentContext(App *app) {
  Book *book = app ? app->GetCurrentBook() : NULL;
  if (!app || !app->IsBookSettingsContext() || !book)
    return false;
  if (!book->GetChapters().empty())
    return true;
  return book->format == FORMAT_EPUB;
}

static bool CanOpenSelectedBookIndex(App *app) {
  Book *book = app ? app->GetSelectedBook() : NULL;
  if (!book)
    return false;
  if (!book->GetChapters().empty())
    return true;
  return book->format == FORMAT_EPUB;
}

static bool CurrentBookUsesLineWrapFixSlot(App *app) {
  Book *book = app ? app->GetCurrentBook() : NULL;
  return app && app->IsBookSettingsContext() && book && book->IsMobiFile();
}

void App::ToggleCurrentBookMobiLineWrapFix() {
  if (!CurrentBookUsesLineWrapFixSlot(this))
    return;
  Book *book = bookcurrent_;
  book->SetMobiLineWrapFix(!book->GetMobiLineWrapFix());
  // The new cleanup changes pagination, but only for this book.
  if (book->GetPageCount() > 0)
    prefs_view_.layout_notice_pending = true;
  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  prefs->Write();
  prefs_view_.view_dirty = true;
}

u8 App::PrefsVisibleButtonCount() const {
  // General settings hide per-book actions like index/bookmarks.
  return prefs_view_.from_book ? PREFS_BUTTON_COUNT : PREFS_BUTTON_INDEX;
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

  prefs_view_.selected_index = PREFS_BUTTON_FONT_CONFIG;
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

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  ClampSelectedIndex(&prefs_view_.selected_index, visibleCount);

  // Dynamic rows depend on current context/book; refresh before drawing.
  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);

  for (int i = 0; i < visibleCount; i++)
    prefsButtons[i].Draw(ts->screenright, i == prefs_view_.selected_index);

  // Draw library button below settings list (without overlapping list rows).
  SyncLibraryButtonLayout(&buttonprefs);
  buttonprefs.Draw(ts->screenright);

  ts->PrintSplash(ts->screenleft);
  if (prefs_view_.from_book && prefs_view_.layout_notice_pending &&
      bookcurrent_ && BookNeedsRelayout(bookcurrent_)) {
    // Settings are saved immediately, but the active book is repaginated only
    // after it is reopened.
    const u8 savedPixelSize = ts->GetPixelSize();
    static const u16 kLayoutNoticeColor =
        RGB565FromU8(188.0f, 36.0f, 36.0f);
    static const u16 kLayoutNoticeBg = RGB565FromU8(255.0f, 255.0f, 255.0f);
    const char *line1 = "reopen book to";
    const char *line2 = "apply changes";
    ts->SetScreen(ts->screenleft);
    ts->SetPixelSize(11);
    const int line1w = ts->GetStringAdvance(line1);
    const int line2w = ts->GetStringAdvance(line2);
    const int line_h = ts->GetHeight();
    const int text_w = std::max(line1w, line2w);
    const int pad_x = 8;
    const int pad_y = 5;
    const int line_gap = 3;
    const int box_w = text_w + pad_x * 2;
    const int box_h = line_h * 2 + line_gap + pad_y * 2;
    const int box_x = (240 - box_w) / 2;
    const int box_y = 90;
    const int line1x = box_x + (box_w - line1w) / 2;
    const int line2x = box_x + (box_w - line2w) / 2;
    const int content_h = line_h * 2 + line_gap;
    const int content_top = box_y + (box_h - content_h) / 2;
    const int line1y = content_top + line_h;
    const int line2y = line1y + line_h + line_gap;
    ts->FillRect((u16)box_x, (u16)box_y, (u16)(box_x + box_w),
                 (u16)(box_y + box_h), kLayoutNoticeBg);
    ts->DrawRect((u16)box_x, (u16)box_y, (u16)(box_x + box_w),
                 (u16)(box_y + box_h), kLayoutNoticeColor);
    ts->SetTextColorOverride(kLayoutNoticeColor);
    ts->SetPen((u16)line1x, (u16)line1y);
    ts->PrintString(line1);
    ts->SetPen((u16)line2x, (u16)line2y);
    ts->PrintString(line2);
    ts->ClearTextColorOverride();
    ts->SetPixelSize(savedPixelSize);
  }

  // restore state
  ts->SetStyle(style);
  ts->SetColorMode(colorMode);
  ts->margin.bottom = savedBottomMargin;
  ts->SetScreen(screen);

  prefs_view_.view_dirty = false;
}

void App::PrefsHandleEvent() {
  u32 keys = hidKeysDown();
  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  ClampSelectedIndex(&prefs_view_.selected_index, visibleCount);

  if (keys & KEY_A) {
    PrefsHandlePress();
  } else if (keys & (KEY_SELECT | KEY_START | KEY_B | KEY_Y)) {
    ShowLibraryView();
  } else if (keys & (key.left | key.l)) {
    if (prefs_view_.selected_index > 0) {
      prefs_view_.selected_index--;
      prefs_view_.view_dirty = true;
    }
  } else if (keys & (key.right | key.r)) {
    if (prefs_view_.selected_index < visibleCount - 1) {
      prefs_view_.selected_index++;
      prefs_view_.view_dirty = true;
    }
  } else if (prefs_view_.selected_index == PREFS_BUTTON_FONTSIZE &&
             (keys & key.up)) {
    PrefsDecreasePixelSize();
  } else if (prefs_view_.selected_index == PREFS_BUTTON_FONTSIZE &&
             (keys & key.down)) {
    PrefsIncreasePixelSize();
  } else if (prefs_view_.selected_index == PREFS_BUTTON_PARASPACING &&
             (keys & key.up)) {
    PrefsDecreaseParaspacing();
  } else if (prefs_view_.selected_index == PREFS_BUTTON_PARASPACING &&
             (keys & key.down)) {
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
  SyncLibraryButtonLayout(&buttonprefs);
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

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  ClampSelectedIndex(&prefs_view_.selected_index, visibleCount);
  for (u8 i = 0; i < visibleCount; i++) {
    if (prefsButtons[i].EnclosesPoint(coord.px, coord.py)) {
      if (i != prefs_view_.selected_index)
        prefs_view_.selected_index = i;

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
      } else {
        PrefsHandlePress();
      }

      break;
    }
  }

  if (prefs_view_.view_dirty)
    PrefsDraw();
}

void App::PrefsIncreasePixelSize() {
  if (ts->pixelsize < 18) {
    ts->SetPixelSize(ts->pixelsize + 1);
    MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    prefs->Write();
  }
}

void App::PrefsDecreasePixelSize() {
  if (ts->pixelsize > 6) {
    ts->SetPixelSize(ts->pixelsize - 1);
    MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    prefs->Write();
  }
}

void App::PrefsIncreaseParaspacing() {
  if (paraspacing < 2) {
    paraspacing++;
    MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    prefs->Write();
  }
}

void App::PrefsDecreaseParaspacing() {
  if (paraspacing > 0) {
    paraspacing--;
    MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    prefs->Write();
  }
}

void App::PrefsFlipOrientation() {
  SetOrientation(!orientation);
  MarkBookLayoutDirty();
  PrefsRefreshButton(PREFS_BUTTON_ORIENTATION);
  prefs->Write();
  // Keep settings view synchronized immediately after rotation toggle.
  if (mode_ == AppMode::Prefs)
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
    snprintf(msg, sizeof(msg), "                        < %d >  ",
             ts->GetPixelSize());
    prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string(msg));
    break;
  case PREFS_BUTTON_PARASPACING:
    snprintf(msg, sizeof(msg), "                         < %d >  ",
             paraspacing);
    prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(std::string(msg));
    break;
  case PREFS_BUTTON_ORIENTATION:
    prefsButtons[PREFS_BUTTON_ORIENTATION].SetLabel2(
        orientation ? std::string("Turned Right") : std::string("Turned Left"));
    break;
  case PREFS_BUTTON_TIME24H:
    if (CurrentBookUsesLineWrapFixSlot(this)) {
      // Reuse this slot in per-book MOBI settings so we avoid adding a ninth
      // row to an already full settings screen.
      prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("line wrap fix"));
      prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
          bookcurrent_->GetMobiLineWrapFix() ? std::string("on")
                                            : std::string("off"));
    } else {
      prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("clock format"));
      prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
          prefs->time24h ? std::string("24h Format")
                         : std::string("12h Format"));
    }
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
        (prefs_view_.from_book && bookcurrent_) ? std::string(">")
                                            : std::string("(open selected book)"));
    break;
  }
  prefs_view_.view_dirty = true;
}

void App::PrefsHandlePress() {
  //! Go to font view or flip orientation.

  if (prefs_view_.selected_index == PREFS_BUTTON_ORIENTATION) {
    PrefsFlipOrientation();
    prefs_view_.view_dirty = true;
    return;
  }

  if (prefs_view_.selected_index == PREFS_BUTTON_TIME24H) {
    if (CurrentBookUsesLineWrapFixSlot(this)) {
      ToggleCurrentBookMobiLineWrapFix();
    } else {
      ToggleClockFormatSetting(prefs);
      PrefsRefreshButton(PREFS_BUTTON_TIME24H);
      prefs_view_.view_dirty = true;
    }
    return;
  }

  if (prefs_view_.selected_index == PREFS_BUTTON_COLORMODE) {
    CycleColorMode(ts);
    PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
    prefs->Write();
    prefs_view_.view_dirty = true;
    return;
  }

  if (prefs_view_.selected_index == PREFS_BUTTON_INDEX) {
    if (CanOpenBookIndexInCurrentContext(this)) {
      ShowChaptersView();
    } else if (CanOpenSelectedBookIndex(this)) {
      OpenBook();
    } else {
      PrintStatus("Index unavailable for this book");
      PrefsRefreshButton(PREFS_BUTTON_INDEX);
      prefs_view_.view_dirty = true;
    }
    return;
  }

  if (prefs_view_.selected_index == PREFS_BUTTON_BOOKMARKS) {
    if (prefs_view_.from_book && bookcurrent_) {
      ShowBookmarksView();
    } else if (!prefs_view_.from_book && browser_.selected_book) {
      OpenBook();
    }
    return;
  }

  if (prefs_view_.selected_index == PREFS_BUTTON_FONT_CONFIG) {
    ShowFontView(AppMode::PrefsFont);
    return;
  }
}
