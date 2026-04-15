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

#include "app/settings_controller.h"
#include "book/book.h"
#include "library/browser_view_utils.h"
#include "ui/button.h"
#include "color_utils.h"
#include "debug_log.h"
#include "parse.h"
#include "settings/prefs.h"
#include "ui/text.h"
#include "ui/text_limits.h"

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

static void ToggleBrowserViewSetting(App *app) {
  if (!app || !app->prefs)
    return;
  app->prefs->browser_view_mode =
      app->prefs->browser_view_mode == BROWSER_VIEW_LIST
          ? BROWSER_VIEW_GALLERY
          : BROWSER_VIEW_LIST;
  if (app->GetSelectedBook()) {
    const int selected_index = app->GetBookIndex(app->GetSelectedBook());
    const int page_size =
        browser_view_utils::PageSize(app->prefs->browser_view_mode);
    if (selected_index >= 0 && page_size > 0)
      app->SetBrowserPageStart((selected_index / page_size) * page_size);
  } else {
    app->SetBrowserPageStart(0);
  }
  app->prefs->Write();
  app->MarkBrowserDirty();
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

static bool CurrentBookUsesTextLayoutSettings(App *app) {
  Book *book = app ? app->GetCurrentBook() : NULL;
  return app && app->IsBookSettingsContext() && book &&
         book->UsesTextLayoutSettings();
}

SettingsController::SettingsController(App &app) : app_(app) {}

void SettingsController::ShowSettingsView(bool from_book) {
  app_.SetBookSettingsContext(from_book);
  app_.SetPrefsLayoutNoticePending(
      from_book && app_.GetCurrentBook() &&
      app_.BookNeedsRelayout(app_.GetCurrentBook()));
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);
  u8 visible_count = PrefsVisibleButtonCount();
  if (visible_count == 0)
    visible_count = 1;
  if (app_.GetPrefsSelectedIndex() >= visible_count)
    app_.SetPrefsSelectedIndex(visible_count - 1);
  app_.SetMode(AppMode::Prefs);
  app_.buttonprefs.Label("library");
  app_.ts->SetScreen(app_.ts->screenright);
  app_.MarkPrefsDirty();
}

void SettingsController::ToggleCurrentBookMobiLineWrapFix() {
  App *app = App::GetInstance();
  if (!CurrentBookUsesLineWrapFixSlot(app))
    return;
  Book *book = app->GetCurrentBook();
  book->SetMobiLineWrapFix(!book->GetMobiLineWrapFix());
  if (book->GetPageCount() > 0)
    app->SetPrefsLayoutNoticePending(true);
  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  app->prefs->Write();
  app->MarkPrefsDirty();
}

u8 SettingsController::PrefsVisibleButtonCount() const {
  return app_.IsBookSettingsContext() ? PREFS_BUTTON_COUNT : PREFS_BUTTON_INDEX;
}

void SettingsController::PrefsInit() {
  App *app = App::GetInstance();
  const std::vector<std::string> labels{
      "font configuration", "font size",    "paragraph spacing",
      "screen orientation", "clock format", "color mode", "library view",
      "index",
      "bookmarks"};

  for (int i = 0; i < PREFS_BUTTON_COUNT; i++) {
    app->prefsButtons[i].Init(app->ts);
    app->prefsButtons[i].SetStyle(BUTTON_STYLE_SETTING);
    app->prefsButtons[i].Resize(230, 36);
    app->prefsButtons[i].SetLabel1(labels[i]);
    PrefsRefreshButton(i);
    app->prefsButtons[i].Move(5, i * 38);
  }

  app->SetPrefsSelectedIndex(PREFS_BUTTON_FONT_CONFIG);
}

void SettingsController::PrefsDraw() {
  App *app = App::GetInstance();
  int colorMode = app->ts->GetColorMode();
  u16 *screen = app->ts->GetScreen();
  int style = app->ts->GetStyle();
  int savedBottomMargin = app->ts->margin.bottom;

  app->ts->margin.bottom = 0;

  app->ts->SetScreen(app->ts->screenright);
  app->ts->SetColorMode(0);
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app->GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app->SetPrefsSelectedIndex(selected_index);

  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);

  for (int i = 0; i < visibleCount; i++)
    app->prefsButtons[i].Draw(app->ts->screenright,
                              i == app->GetPrefsSelectedIndex());

  SyncLibraryButtonLayout(&app->buttonprefs);
  app->buttonprefs.Draw(app->ts->screenright);

  app->ts->PrintSplash(app->ts->screenleft);
  if (app->IsBookSettingsContext() && app->IsPrefsLayoutNoticePending() &&
      app->GetCurrentBook() && app->BookNeedsRelayout(app->GetCurrentBook())) {
    const u8 savedPixelSize = app->ts->GetPixelSize();
    static const u16 kLayoutNoticeColor = RGB565FromU8(188.0f, 36.0f, 36.0f);
    static const u16 kLayoutNoticeBg = RGB565FromU8(255.0f, 255.0f, 255.0f);
    const char *line1 = "reopen book to";
    const char *line2 = "apply changes";
    app->ts->SetScreen(app->ts->screenleft);
    app->ts->SetPixelSize(11);
    const int line1w = app->ts->GetStringAdvance(line1);
    const int line2w = app->ts->GetStringAdvance(line2);
    const int line_h = app->ts->GetHeight();
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
    app->ts->FillRect((u16)box_x, (u16)box_y, (u16)(box_x + box_w),
                      (u16)(box_y + box_h), kLayoutNoticeBg);
    app->ts->DrawRect((u16)box_x, (u16)box_y, (u16)(box_x + box_w),
                      (u16)(box_y + box_h), kLayoutNoticeColor);
    app->ts->SetTextColorOverride(kLayoutNoticeColor);
    app->ts->SetPen((u16)line1x, (u16)line1y);
    app->ts->PrintString(line1);
    app->ts->SetPen((u16)line2x, (u16)line2y);
    app->ts->PrintString(line2);
    app->ts->ClearTextColorOverride();
    app->ts->SetPixelSize(savedPixelSize);
  }

  app->ts->SetStyle(style);
  app->ts->SetColorMode(colorMode);
  app->ts->margin.bottom = savedBottomMargin;
  app->ts->SetScreen(screen);

  app->SetPrefsDirty(false);
}

void SettingsController::PrefsHandleEvent() {
  App *app = App::GetInstance();
  u32 keys = hidKeysDown();
#ifdef DSLIBRIS_DEBUG
  static int s_prefs_keys_budget = 48;
  if (s_prefs_keys_budget > 0 && keys) {
    DBG_LOGF(app, "PREFS keys=0x%08lx sel=%d mode=%d", (unsigned long)keys,
             app->GetPrefsSelectedIndex(), (int)app->GetMode());
    s_prefs_keys_budget--;
  }
#endif
  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app->GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app->SetPrefsSelectedIndex(selected_index);

  if (keys & KEY_A) {
    PrefsHandlePress();
    if (app->GetMode() != AppMode::Prefs)
      return;
  } else if (keys & (KEY_SELECT | KEY_START | KEY_B | KEY_Y)) {
    app->ShowLibraryView();
  } else if (keys & (app->key.left | app->key.l)) {
    if (app->GetPrefsSelectedIndex() > 0) {
      app->SetPrefsSelectedIndex(app->GetPrefsSelectedIndex() - 1);
      app->MarkPrefsDirty();
    }
  } else if (keys & (app->key.right | app->key.r)) {
    if (app->GetPrefsSelectedIndex() < visibleCount - 1) {
      app->SetPrefsSelectedIndex(app->GetPrefsSelectedIndex() + 1);
      app->MarkPrefsDirty();
    }
  } else if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_FONTSIZE &&
             (keys & app->key.up)) {
    PrefsDecreasePixelSize();
  } else if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_FONTSIZE &&
             (keys & app->key.down)) {
    PrefsIncreasePixelSize();
  } else if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_PARASPACING &&
             (keys & app->key.up)) {
    PrefsDecreaseParaspacing();
  } else if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_PARASPACING &&
             (keys & app->key.down)) {
    PrefsIncreaseParaspacing();
  } else if (keys & KEY_TOUCH) {
    PrefsHandleTouch();
  }
}

void SettingsController::PrefsHandleTouch() {
  App *app = App::GetInstance();
  const AppMode mode_before_touch = app->GetMode();
  touchPosition coord = app->TouchRead();
  const int footerX = (int)coord.px;
  const int footerY = (int)coord.py;

  SyncLibraryButtonLayout(&app->buttonprefs);
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

  if (enclosesWithSlack(app->buttonprefs, footerX, footerY)) {
    app->ShowLibraryView();
    return;
  }

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app->GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app->SetPrefsSelectedIndex(selected_index);
  for (u8 i = 0; i < visibleCount; i++) {
    if (app->prefsButtons[i].EnclosesPoint(coord.px, coord.py)) {
      if (i != app->GetPrefsSelectedIndex())
        app->SetPrefsSelectedIndex(i);

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
        if (app->GetMode() != mode_before_touch)
          return;
      }

      break;
    }
  }

  if (app->IsPrefsDirty())
    PrefsDraw();
}

void SettingsController::PrefsIncreasePixelSize() {
  App *app = App::GetInstance();
  if (app->IsBookSettingsContext() && !CurrentBookUsesTextLayoutSettings(app))
    return;
  if (app->ts->pixelsize < kTextPixelSizeMax) {
    app->ts->SetPixelSize(app->ts->pixelsize + 1);
    app->MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app->prefs->Write();
  }
}

void SettingsController::PrefsDecreasePixelSize() {
  App *app = App::GetInstance();
  if (app->IsBookSettingsContext() && !CurrentBookUsesTextLayoutSettings(app))
    return;
  if (app->ts->pixelsize > kTextPixelSizeMin) {
    app->ts->SetPixelSize(app->ts->pixelsize - 1);
    app->MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app->prefs->Write();
  }
}

void SettingsController::PrefsIncreaseParaspacing() {
  App *app = App::GetInstance();
  if (app->IsBookSettingsContext() && !CurrentBookUsesTextLayoutSettings(app))
    return;
  if (app->paraspacing < 2) {
    app->paraspacing++;
    app->MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app->prefs->Write();
  }
}

void SettingsController::PrefsDecreaseParaspacing() {
  App *app = App::GetInstance();
  if (app->IsBookSettingsContext() && !CurrentBookUsesTextLayoutSettings(app))
    return;
  if (app->paraspacing > 0) {
    app->paraspacing--;
    app->MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app->prefs->Write();
  }
}

void SettingsController::PrefsFlipOrientation() {
  App *app = App::GetInstance();
  app->SetOrientation(!app->orientation);
  app->MarkBookLayoutDirty();
  PrefsRefreshButton(PREFS_BUTTON_ORIENTATION);
  app->prefs->Write();
  if (app->GetMode() == AppMode::Prefs)
    PrefsDraw();
}

void SettingsController::PrefsRefreshButton(int index) {
  App *app = App::GetInstance();
  char msg[64];
  switch (index) {
  case PREFS_BUTTON_FONT_CONFIG:
    app->prefsButtons[PREFS_BUTTON_FONT_CONFIG].SetLabel2(
        std::string("open menu >"));
    break;
  case PREFS_BUTTON_FONTSIZE:
    if (app->IsBookSettingsContext() && app->GetCurrentBook() &&
        !app->GetCurrentBook()->UsesTextLayoutSettings()) {
      app->prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string("(PDF fixed)"));
    } else {
      snprintf(msg, sizeof(msg), "                        < %d >  ",
               app->ts->GetPixelSize());
      app->prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string(msg));
    }
    break;
  case PREFS_BUTTON_PARASPACING:
    if (app->IsBookSettingsContext() && app->GetCurrentBook() &&
        !app->GetCurrentBook()->UsesTextLayoutSettings()) {
      app->prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(
          std::string("(PDF fixed)"));
    } else {
      snprintf(msg, sizeof(msg), "                         < %d >  ", app->paraspacing);
      app->prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(std::string(msg));
    }
    break;
  case PREFS_BUTTON_ORIENTATION:
    app->prefsButtons[PREFS_BUTTON_ORIENTATION].SetLabel2(
        app->orientation ? std::string("Turned Right") : std::string("Turned Left"));
    break;
  case PREFS_BUTTON_TIME24H:
    if (CurrentBookUsesLineWrapFixSlot(app)) {
      app->prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("line wrap fix"));
      app->prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
          app->GetCurrentBook()->GetMobiLineWrapFix() ? std::string("on")
                                                      : std::string("off"));
    } else {
      app->prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("clock format"));
      app->prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
          app->prefs->time24h ? std::string("24h Format")
                              : std::string("12h Format"));
    }
    break;
  case PREFS_BUTTON_COLORMODE: {
    int mode = app->ts->GetColorMode();
    if (mode == 0)
      app->prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Normal"));
    else if (mode == 1)
      app->prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Dark"));
    else
      app->prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Sepia"));
    break;
  }
  case PREFS_BUTTON_LIBRARY_VIEW:
    app->prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel2(
        std::string(browser_view_utils::Label(app->prefs->browser_view_mode)));
    break;
  case PREFS_BUTTON_INDEX:
    if (CanOpenBookIndexInCurrentContext(app)) {
      app->prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string(">"));
    } else if (CanOpenSelectedBookIndex(app)) {
      app->prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(
          std::string("(open selected book)"));
    } else {
      app->prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string("(not available)"));
    }
    break;
  case PREFS_BUTTON_BOOKMARKS:
    if (app->IsBookSettingsContext() && app->GetCurrentBook() &&
        !app->GetCurrentBook()->SupportsBookmarks()) {
      app->prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
          std::string("(PDF disabled)"));
    } else {
      app->prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
          (app->IsBookSettingsContext() && app->GetCurrentBook())
              ? std::string(">")
              : std::string("(open selected book)"));
    }
    break;
  }
  app->MarkPrefsDirty();
}

void SettingsController::PrefsHandlePress() {
  App *app = App::GetInstance();

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_ORIENTATION) {
    PrefsFlipOrientation();
    app->MarkPrefsDirty();
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_TIME24H) {
    if (CurrentBookUsesLineWrapFixSlot(app)) {
      ToggleCurrentBookMobiLineWrapFix();
    } else {
      ToggleClockFormatSetting(app->prefs);
      PrefsRefreshButton(PREFS_BUTTON_TIME24H);
      app->MarkPrefsDirty();
    }
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_COLORMODE) {
    CycleColorMode(app->ts);
    PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
    app->prefs->Write();
    app->MarkPrefsDirty();
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_LIBRARY_VIEW) {
    ToggleBrowserViewSetting(app);
    PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
    app->MarkPrefsDirty();
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_INDEX) {
    Book *current = app->GetCurrentBook();
    Book *selected = app->GetSelectedBook();
    DBG_LOGF(
        app,
        "PREFS index press from_book=%d cur=%p sel=%p cur_fmt=%d sel_fmt=%d cur_ch=%u sel_ch=%u",
        app->IsBookSettingsContext() ? 1 : 0, (void *)current, (void *)selected,
        current ? (int)current->format : -1, selected ? (int)selected->format : -1,
        current ? (unsigned)current->GetChapters().size() : 0u,
        selected ? (unsigned)selected->GetChapters().size() : 0u);
    DBG_LOG(app, "PREFS index eval begin");
    const bool can_open_current = CanOpenBookIndexInCurrentContext(app);
    const bool can_open_selected = CanOpenSelectedBookIndex(app);
    DBG_LOGF(app, "PREFS index eval current=%d selected=%d",
             can_open_current ? 1 : 0, can_open_selected ? 1 : 0);
    if (can_open_current) {
      DBG_LOG(app, "PREFS index action=open current");
      app->ShowChaptersView();
      DBG_LOG(app, "PREFS index action=open current done");
    } else if (can_open_selected) {
      DBG_LOG(app, "PREFS index action=open selected");
      app->OpenBook();
      DBG_LOG(app, "PREFS index action=open selected done");
    } else {
      DBG_LOG(app, "PREFS index action=unavailable");
      app->PrintStatus("Index unavailable for this book");
      PrefsRefreshButton(PREFS_BUTTON_INDEX);
      app->MarkPrefsDirty();
    }
    DBG_LOG(app, "PREFS index eval end");
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_BOOKMARKS) {
    if (app->IsBookSettingsContext() && app->GetCurrentBook() &&
        !app->GetCurrentBook()->SupportsBookmarks()) {
      app->PrintStatus("Bookmarks unavailable for PDF");
    } else if (app->IsBookSettingsContext() && app->GetCurrentBook()) {
      app->ShowBookmarksView();
    } else if (!app->IsBookSettingsContext() && app->GetSelectedBook()) {
      app->OpenBook();
    }
    return;
  }

  if (app->GetPrefsSelectedIndex() == PREFS_BUTTON_FONT_CONFIG) {
    app->ShowFontView(AppMode::PrefsFont);
    return;
  }
}

void App::ToggleCurrentBookMobiLineWrapFix() {
  settings_controller_->ToggleCurrentBookMobiLineWrapFix();
}

u8 App::PrefsVisibleButtonCount() const {
  return settings_controller_->PrefsVisibleButtonCount();
}

void App::PrefsInit() { settings_controller_->PrefsInit(); }

void App::PrefsDraw() { settings_controller_->PrefsDraw(); }

void App::PrefsHandleEvent() { settings_controller_->PrefsHandleEvent(); }

void App::PrefsHandleTouch() { settings_controller_->PrefsHandleTouch(); }

void App::PrefsIncreasePixelSize() { settings_controller_->PrefsIncreasePixelSize(); }

void App::PrefsDecreasePixelSize() { settings_controller_->PrefsDecreasePixelSize(); }

void App::PrefsIncreaseParaspacing() {
  settings_controller_->PrefsIncreaseParaspacing();
}

void App::PrefsDecreaseParaspacing() {
  settings_controller_->PrefsDecreaseParaspacing();
}

void App::PrefsFlipOrientation() { settings_controller_->PrefsFlipOrientation(); }

void App::PrefsRefreshButton(int index) {
  settings_controller_->PrefsRefreshButton(index);
}

void App::PrefsHandlePress() { settings_controller_->PrefsHandlePress(); }
