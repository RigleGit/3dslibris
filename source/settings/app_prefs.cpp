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
#include "shared/screen_dimensions.h"

#include <algorithm>
#include <dirent.h>
#include <sys/types.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <3ds.h>

#include "app/settings_controller.h"
#include "book/book.h"
#include "book/book_renderer.h"
#include "library/browser_view_utils.h"
#include "utf8proc.h"
#include "ui/button.h"
#include "ui/ui_button_skin.h"
#include "shared/color_utils.h"
#include "shared/debug_log.h"
#include "parse.h"
#include "shared/path_constants.h"
#include "settings/prefs.h"
#include "settings/prefs_button_context_utils.h"
#include "ui/text.h"
#include "ui/screen_layout_constants.h"
#include "ui/text_limits.h"

static const int PREFS_LIBRARY_BTN_X = 130;
static const int PREFS_LIBRARY_BTN_Y = 286;
static const int PREFS_LIBRARY_BTN_W = 104;
static const int PREFS_LIBRARY_BTN_H = 26;


static const int kPage2Buttons[] = {
    PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN,
    PREFS_BUTTON_PUBLISHER_FONTSIZE,
    PREFS_BUTTON_RESET_DEFAULTS,
    PREFS_BUTTON_CLEAR_CACHE,
};
static const int kPage2ButtonCount = 4;
static const int PREFS_ROW_X = 5;
static const int PREFS_ROW_W = 230;
static const u32 kGoToPageCoarseStep = 10;

static bool CurrentBookShowsLineWrapFix(Book *book, bool is_book_ctx);


static u8 NormalizeVisibleCount(u8 count) { return count == 0 ? 1 : count; }

static void ClampSelectedIndex(int *selected, u8 visibleCount) {
  if (!selected || visibleCount == 0)
    return;
  if (*selected >= visibleCount)
    *selected = visibleCount - 1;
}


static void SyncLibraryButtonLayout(Button *button, bool paged, bool book_ctx) {
  if (!button)
    return;
  if (paged) {
    button->Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
    button->Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
  } else if (book_ctx) {
    button->Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
    button->Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  } else {
    button->Move(PREFS_LIBRARY_BTN_X, PREFS_LIBRARY_BTN_Y);
    button->Resize(PREFS_LIBRARY_BTN_W, PREFS_LIBRARY_BTN_H);
  }
}

static void ToggleClockFormatSetting(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->time24h = !prefs->time24h;
  prefs->Write();
}

static void CycleColorMode(Text *ts, App *app) {
  if (!ts)
    return;
  int mode = ts->GetColorMode();
  int next = (mode + 1) % 6;
  ts->SetColorMode(next);
  UiButtonSkin_SetColorMode(next);
  if (app) {
    app->colorMode = next;
    ts->MarkAllScreensDirty();
    DBG_LOGF(app, "CycleColorMode: %d -> %d", mode, next);
  }
}

static void ToggleBrowserViewSetting(App *app) {
  if (!app || !app->prefs.get())
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
  app->ResetBrowserMarquee();
  app->MarkBrowserDirty();
  app->LoadVisibleBrowserCoverCaches();
}

static bool CanOpenBookIndexInCurrentContext(Book *book, bool is_book_ctx) {
  if (!is_book_ctx || !book)
    return false;
  if (!book->GetChapters().empty())
    return true;
  return book->format == FORMAT_EPUB;
}

static bool CanOpenSelectedBookIndex(Book *book) {
  if (!book || book->IsBrowserFolder())
    return false;
  if (!book->GetChapters().empty())
    return true;
  return book->format == FORMAT_EPUB;
}

static bool CurrentBookUsesLineWrapFixSlot(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->IsMobiFile();
}

static bool CurrentBookUsesReadingDirectionSlot(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->IsFixedLayout();
}

static bool CurrentBookShowsLineWrapFix(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && (book->IsMobiFile() || book->IsFixedLayout());
}

static bool CurrentBookUsesTextLayoutSettings(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->UsesTextLayoutSettings();
}

static bool CurrentBookCanGoToPage(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->GetPageCount() > 0;
}

static void ToggleFixedLayoutReadingDirection(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->fixed_layout_rtl = !prefs->fixed_layout_rtl;
  prefs->Write();
}

static void ToggleCirclePadPageTurnSetting(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->circle_pad_page_turn = !prefs->circle_pad_page_turn;
  prefs->Write();
}

SettingsController::SettingsController(App &app)
    : app_(app), go_to_page_dialog_(app), prefs_general_page_(0) {}

int SettingsController::EffectiveVisibleCount() const {
  if (!app_.IsBookSettingsContext() && prefs_general_page_ == 1)
    return kPage2ButtonCount;
  const bool is_book_ctx = app_.IsBookSettingsContext();
  return (int)settings::VisiblePrefsButtonCount(
      is_book_ctx, CurrentBookShowsLineWrapFix(app_.GetCurrentBook(), is_book_ctx));
}

int SettingsController::EffectiveButtonForSlot(int slot) const {
  if (!app_.IsBookSettingsContext() && prefs_general_page_ == 1)
    return (slot >= 0 && slot < kPage2ButtonCount) ? kPage2Buttons[slot] : kPage2Buttons[0];
  const bool is_book_ctx = app_.IsBookSettingsContext();
  return settings::PrefsButtonForVisibleSlot(
      is_book_ctx, CurrentBookShowsLineWrapFix(app_.GetCurrentBook(), is_book_ctx), (u8)slot);
}

void SettingsController::GoToPrefsPage(int page) {
  prefs_general_page_ = page;
  const int new_count = EffectiveVisibleCount();
  int new_sel = page == 1 ? 0 : (new_count > 0 ? new_count - 1 : 0);
  app_.SetPrefsSelectedIndex(new_sel);
  app_.MarkPrefsDirty();
}

void SettingsController::ShowSettingsView(bool from_book) {
  prefs_general_page_ = 0;
  go_to_page_dialog_.Close();
  app_.SetBookSettingsContext(from_book);
  app_.SetPrefsLayoutNoticePending(
      from_book && app_.GetCurrentBook() &&
      app_.BookNeedsRelayout(app_.GetCurrentBook()));

  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);
  PrefsRefreshButton(PREFS_BUTTON_CLEAR_CACHE);
  
  u8 visible_count = PrefsVisibleButtonCount();
  if (visible_count == 0)
    visible_count = 1;
  if (app_.GetPrefsSelectedIndex() >= visible_count)
    app_.SetPrefsSelectedIndex(visible_count - 1);
  app_.SetMode(AppMode::Prefs);
  app_.buttonprefs.Label(from_book ? "back" : "library");
  app_.ts->SetScreen(app_.ts->screenright);
  app_.MarkPrefsDirty();
}

void SettingsController::ToggleCurrentBookMobiLineWrapFix() {
  Book *book = app_.GetCurrentBook();
  if (!CurrentBookUsesLineWrapFixSlot(book, app_.IsBookSettingsContext()))
    return;
  book->SetMobiLineWrapFix(!book->GetMobiLineWrapFix());
  if (book->GetPageCount() > 0)
    app_.SetPrefsLayoutNoticePending(true);
  PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
  app_.prefs->Write();
  app_.MarkPrefsDirty();
}


u8 SettingsController::PrefsVisibleButtonCount() const {
  return (u8)EffectiveVisibleCount();
}

void SettingsController::PrefsInit() {
  const std::vector<std::string> labels{
      "font configuration", "font size",    "paragraph spacing",
      "screen orientation", "clock format", "color mode", "library view",
      "index",              "bookmarks",    "reset settings", "clear cache",
      "publisher font sizes"};

  for (int i = 0; i < PREFS_BUTTON_COUNT; i++) {
    app_.prefsButtons[i].Init(app_.ts.get());
    app_.prefsButtons[i].SetStyle(BUTTON_STYLE_SETTING);
    app_.prefsButtons[i].Resize(230, 36);
    app_.prefsButtons[i].SetLabel1(labels[i]);
    PrefsRefreshButton(i);
    app_.prefsButtons[i].Move(5, i * 38);
  }

  app_.SetPrefsSelectedIndex(PREFS_BUTTON_FONT_CONFIG);
  prefs_general_page_ = 0;

  button_prefs_page_nav_.Init(app_.ts.get());
  button_prefs_page_nav_.SetStyle(BUTTON_STYLE_BOOK);
  button_prefs_page_nav_.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);

  button_prefs_library_.Init(app_.ts.get());
  button_prefs_library_.SetStyle(BUTTON_STYLE_BOOK);
  button_prefs_library_.Label("library");
  button_prefs_library_.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
}

void SettingsController::PrefsDraw() {
  Text *ts = app_.ts.get();
  int colorMode = ts->GetColorMode();
  u16 *screen = ts->GetScreen();
  int style = ts->GetStyle();
  int savedBottomMargin = ts->margin.bottom;

  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  app_.DrawBottomGradientBackground();

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app_.GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app_.SetPrefsSelectedIndex(selected_index);

  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
  PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);

  for (int slot = 0; slot < visibleCount; slot++) {
    const int button_id = EffectiveButtonForSlot(slot);
    app_.prefsButtons[button_id].Move(5, slot * 38);
    app_.prefsButtons[button_id].Draw(ts->screenright,
                                      slot == app_.GetPrefsSelectedIndex());
  }

  if (go_to_page_dialog_.IsOpen())
    go_to_page_dialog_.Draw();

  const bool paged = !app_.IsBookSettingsContext();
  const bool book_ctx = app_.IsBookSettingsContext();
  SyncLibraryButtonLayout(&app_.buttonprefs, paged, book_ctx);
  app_.buttonprefs.Draw(ts->screenright);
  if (book_ctx) {
    button_prefs_library_.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
    button_prefs_library_.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
    button_prefs_library_.Draw(ts->screenright);
  } else if (paged) {
    if (prefs_general_page_ == 0) {
      button_prefs_page_nav_.Label("next");
      button_prefs_page_nav_.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
    } else {
      button_prefs_page_nav_.Label("prev");
      button_prefs_page_nav_.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
    }
    button_prefs_page_nav_.Draw(ts->screenright);
  }

  ts->PrintSplash(ts->screenleft);
  if (app_.IsBookSettingsContext() && app_.IsPrefsLayoutNoticePending() &&
      app_.GetCurrentBook() && app_.BookNeedsRelayout(app_.GetCurrentBook())) {
    const u8 savedPixelSize = ts->GetPixelSize();
    static const u16 kLayoutNoticeColor = RGB565FromU8(188.0f, 36.0f, 36.0f);
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
    const int box_x = (screen_dims::kTopScreenWidthPx - box_w) / 2;
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

  ts->SetStyle(style);
  ts->SetColorMode(colorMode);
  ts->margin.bottom = savedBottomMargin;
  ts->SetScreen(screen);

  app_.SetPrefsDirty(false);
}

void SettingsController::PrefsHandleEvent() {
  u32 keys = hidKeysDown();
  u32 held = hidKeysHeld();
#ifdef DSLIBRIS_DEBUG
  static int s_prefs_keys_budget = 48;
  if (s_prefs_keys_budget > 0 && keys) {
    DBG_LOGF((&app_), "PREFS keys=0x%08lx sel=%d mode=%d", (unsigned long)keys,
             app_.GetPrefsSelectedIndex(), (int)app_.GetMode());
    s_prefs_keys_budget--;
  }
#endif
  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app_.GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app_.SetPrefsSelectedIndex(selected_index);
  const int selected_button = EffectiveButtonForSlot(app_.GetPrefsSelectedIndex());

  if (go_to_page_dialog_.IsOpen()) {
    if (keys & KEY_A) {
      go_to_page_dialog_.Confirm();
      return;
    }
    if (keys & (KEY_B | KEY_SELECT | KEY_START | KEY_Y)) {
      go_to_page_dialog_.Close();
      if (app_.IsPrefsDirty())
        PrefsDraw();
      return;
    }
    if (keys & app_.key.left) {
      go_to_page_dialog_.AdjustTarget(-1);
    } else if (keys & app_.key.right) {
      go_to_page_dialog_.AdjustTarget(1);
    } else if (keys & (app_.key.up | app_.key.l)) {
      go_to_page_dialog_.AdjustTarget(-(int)kGoToPageCoarseStep);
    } else if (keys & (app_.key.down | app_.key.r)) {
      go_to_page_dialog_.AdjustTarget((int)kGoToPageCoarseStep);
    }
    if ((keys & KEY_TOUCH) || (held & KEY_TOUCH))
      go_to_page_dialog_.HandleTouch((keys & KEY_TOUCH) != 0);
    if (app_.IsPrefsDirty())
      PrefsDraw();
    return;
  }

  if (keys & KEY_A) {
    PrefsHandlePress();
    if (app_.GetMode() != AppMode::Prefs)
      return;
  } else if (app_.IsBookSettingsContext() && (keys & KEY_START)) {
    app_.ShowLibraryView();
    app_.prefs->Write();
  } else if (keys & (KEY_SELECT | KEY_B | KEY_Y)) {
    app_.ReturnFromPrefs();
  } else if (keys & (app_.key.left | app_.key.l)) {
    if (app_.GetPrefsSelectedIndex() > 0) {
      app_.SetPrefsSelectedIndex(app_.GetPrefsSelectedIndex() - 1);
      app_.MarkPrefsDirty();
    } else if (!app_.IsBookSettingsContext() && prefs_general_page_ == 1) {
      GoToPrefsPage(0);
    }
  } else if (keys & (app_.key.right | app_.key.r)) {
    if (app_.GetPrefsSelectedIndex() < visibleCount - 1) {
      app_.SetPrefsSelectedIndex(app_.GetPrefsSelectedIndex() + 1);
      app_.MarkPrefsDirty();
    } else if (!app_.IsBookSettingsContext() && prefs_general_page_ == 0) {
      GoToPrefsPage(1);
    }
  } else if (selected_button == PREFS_BUTTON_FONTSIZE &&
             (keys & app_.key.up)) {
    PrefsDecreasePixelSize();
  } else if (selected_button == PREFS_BUTTON_FONTSIZE &&
             (keys & app_.key.down)) {
    PrefsIncreasePixelSize();
  } else if (selected_button == PREFS_BUTTON_PARASPACING &&
             (keys & app_.key.up)) {
    PrefsDecreaseParaspacing();
  } else if (selected_button == PREFS_BUTTON_PARASPACING &&
             (keys & app_.key.down)) {
    PrefsIncreaseParaspacing();
  } else if (keys & KEY_TOUCH) {
    PrefsHandleTouch();
  }
}

void SettingsController::PrefsHandleTouch() {
  const AppMode mode_before_touch = app_.GetMode();
  touchPosition coord = app_.TouchRead();
  const int footerX = (int)coord.px;
  const int footerY = (int)coord.py;

  const bool book_ctx_touch = app_.IsBookSettingsContext();
  SyncLibraryButtonLayout(&app_.buttonprefs, !book_ctx_touch, book_ctx_touch);
  if (book_ctx_touch) {
    button_prefs_library_.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
    button_prefs_library_.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
  }
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

  if (enclosesWithSlack(app_.buttonprefs, footerX, footerY)) {
    app_.ReturnFromPrefs();
    return;
  }

  if (book_ctx_touch &&
      enclosesWithSlack(button_prefs_library_, footerX, footerY)) {
    app_.ShowLibraryView();
    app_.prefs->Write();
    return;
  }

  if (!book_ctx_touch &&
      enclosesWithSlack(button_prefs_page_nav_, footerX, footerY)) {
    GoToPrefsPage(prefs_general_page_ == 0 ? 1 : 0);
    return;
  }

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app_.GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app_.SetPrefsSelectedIndex(selected_index);
  for (u8 i = 0; i < visibleCount; i++) {
    const int button_id = EffectiveButtonForSlot(i);
    if (app_.prefsButtons[button_id].EnclosesPoint(coord.px, coord.py)) {
      if (i != app_.GetPrefsSelectedIndex())
        app_.SetPrefsSelectedIndex(i);

      if (button_id == PREFS_BUTTON_FONTSIZE) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreasePixelSize();
        } else {
          PrefsDecreasePixelSize();
        }
      } else if (button_id == PREFS_BUTTON_PARASPACING) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreaseParaspacing();
        } else {
          PrefsDecreaseParaspacing();
        }
      } else {
        PrefsHandlePress();
        if (app_.GetMode() != mode_before_touch)
          return;
      }

      break;
    }
  }

  if (app_.IsPrefsDirty())
    PrefsDraw();
}

void SettingsController::PrefsIncreasePixelSize() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  if (app_.ts->pixelsize < kTextPixelSizeMax) {
    app_.ts->SetPixelSize(app_.ts->pixelsize + 1);
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app_.prefs->Write();
  }
}

void SettingsController::PrefsDecreasePixelSize() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  if (app_.ts->pixelsize > kTextPixelSizeMin) {
    app_.ts->SetPixelSize(app_.ts->pixelsize - 1);
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app_.prefs->Write();
  }
}

void SettingsController::PrefsIncreaseParaspacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  if (app_.paraspacing < 2) {
    app_.paraspacing++;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app_.prefs->Write();
  }
}

void SettingsController::PrefsDecreaseParaspacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  if (app_.paraspacing > 0) {
    app_.paraspacing--;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app_.prefs->Write();
  }
}

void SettingsController::PrefsFlipOrientation() {
  app_.SetOrientation(!app_.orientation);
  app_.MarkBookLayoutDirty();
  PrefsRefreshButton(PREFS_BUTTON_ORIENTATION);
  app_.prefs->Write();
  if (app_.GetMode() == AppMode::Prefs)
    PrefsDraw();
}

void SettingsController::PrefsRefreshButton(int index) {
  const bool is_book_ctx = app_.IsBookSettingsContext();
  Book *book = app_.GetCurrentBook();
  char msg[64];
  switch (index) {
  case PREFS_BUTTON_FONT_CONFIG:
    app_.prefsButtons[PREFS_BUTTON_FONT_CONFIG].SetLabel2(
        std::string("open menu >"));
    break;
  case PREFS_BUTTON_FONTSIZE:
    if (is_book_ctx && book && !book->UsesTextLayoutSettings()) {
      app_.prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string("(PDF fixed)"));
    } else {
      snprintf(msg, sizeof(msg), "                        < %d >  ",
               app_.ts->GetPixelSize());
      app_.prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(std::string(msg));
    }
    break;
  case PREFS_BUTTON_PARASPACING:
    if (is_book_ctx && book && !book->UsesTextLayoutSettings()) {
      app_.prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(
          std::string("(PDF fixed)"));
    } else {
      snprintf(msg, sizeof(msg), "                         < %d >  ", app_.paraspacing);
      app_.prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(std::string(msg));
    }
    break;
  case PREFS_BUTTON_ORIENTATION:
    app_.prefsButtons[PREFS_BUTTON_ORIENTATION].SetLabel2(
        app_.orientation ? std::string("Turned Right") : std::string("Turned Left"));
    break;
  case PREFS_BUTTON_TIME24H:
    if (CurrentBookCanGoToPage(book, is_book_ctx)) {
      app_.prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("go to page"));
      if (book->GetPageCount() <= 1) {
        app_.prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
            std::string("(single page)"));
      } else {
        snprintf(msg, sizeof(msg), "Pg %d / %d >", book->GetPosition() + 1,
                 book->GetPageCount());
        app_.prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(std::string(msg));
      }
    } else {
      app_.prefsButtons[PREFS_BUTTON_TIME24H].SetLabel1(std::string("clock format"));
      app_.prefsButtons[PREFS_BUTTON_TIME24H].SetLabel2(
          app_.prefs->time24h ? std::string("24h Format")
                              : std::string("12h Format"));
    }
    break;
  case PREFS_BUTTON_COLORMODE: {
    int mode = app_.ts->GetColorMode();
    app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel1(std::string("color mode"));
    switch (mode) {
    case 0:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Light"));
      break;
    case 1:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Dark"));
      break;
    case 2:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Sepia"));
      break;
    case 3:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("True Light"));
      break;
    case 4:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("True Dark"));
      break;
    case 5:
      app_.prefsButtons[PREFS_BUTTON_COLORMODE].SetLabel2(std::string("Dark Sepia"));
      break;
    }
    break;
  }
  case PREFS_BUTTON_LIBRARY_VIEW:
    if (CurrentBookUsesLineWrapFixSlot(book, is_book_ctx)) {
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel1(
          std::string("line wrap fix"));
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel2(
          book->GetMobiLineWrapFix() ? std::string("on") : std::string("off"));
    } else if (CurrentBookUsesReadingDirectionSlot(book, is_book_ctx)) {
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel1(
          std::string("reading direction"));
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel2(
          app_.prefs->fixed_layout_rtl ? std::string("Right to left")
                                       : std::string("Left to right"));
    } else {
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel1(
          std::string("library view"));
      app_.prefsButtons[PREFS_BUTTON_LIBRARY_VIEW].SetLabel2(
          std::string(browser_view_utils::Label(app_.prefs->browser_view_mode)));
    }
    break;
  case PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN:
    app_.prefsButtons[PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN].SetLabel1(
        std::string("circle pad pages"));
    app_.prefsButtons[PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN].SetLabel2(
        app_.prefs->circle_pad_page_turn ? std::string("on")
                                         : std::string("off"));
    break;
  case PREFS_BUTTON_INDEX:
    if (CanOpenBookIndexInCurrentContext(book, is_book_ctx)) {
      app_.prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string(">"));
    } else if (CanOpenSelectedBookIndex(app_.GetSelectedBook())) {
      app_.prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(
          std::string("(open selected book)"));
    } else {
      app_.prefsButtons[PREFS_BUTTON_INDEX].SetLabel2(std::string("(not available)"));
    }
    break;
  case PREFS_BUTTON_BOOKMARKS:
    if (is_book_ctx && book && !book->SupportsBookmarks()) {
      app_.prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
          std::string("(PDF disabled)"));
    } else {
      app_.prefsButtons[PREFS_BUTTON_BOOKMARKS].SetLabel2(
          (is_book_ctx && book) ? std::string(">")
                                : std::string("(open selected book)"));
    }
    break;
  case PREFS_BUTTON_PUBLISHER_FONTSIZE:
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_FONTSIZE].SetLabel2(
        app_.prefs->respect_publisher_font_size ? std::string("On")
                                                : std::string("Off"));
    break;
  case PREFS_BUTTON_RESET_DEFAULTS:
    app_.prefsButtons[PREFS_BUTTON_RESET_DEFAULTS].SetLabel2(std::string("restore defaults >"));
    break;
  case PREFS_BUTTON_CLEAR_CACHE:
    app_.prefsButtons[PREFS_BUTTON_CLEAR_CACHE].SetLabel2(std::string("delete all caches >"));
    break;
  }
  app_.MarkPrefsDirty();
}

// On macOS/Azahar, APFS returns NFD filenames from readdir, but the 3DS FS
// service stored the file under NFC UTF-16. Normalize d_name to NFC so
// libctru's UTF-8→UTF-16 conversion produces a matching codepoint.
static void RemoveFromDir(const char *dir, const char *name) {
  char path[512];
  uint8_t *nfc = nullptr;
  utf8proc_ssize_t nfc_len = utf8proc_map(
      (const uint8_t *)name, 0, &nfc,
      (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE));
  const char *safe_name = (nfc_len >= 0 && nfc) ? (const char *)nfc : name;
  snprintf(path, sizeof(path), "%s/%s", dir, safe_name);
  int rc = remove(path);
  free(nfc);
#ifdef DSLIBRIS_DEBUG
  if (rc != 0) {
    App *app_dbg = App::GetInstance();
    if (app_dbg)
      DBG_LOGF(app_dbg, "DeleteDirContents: remove failed path=%s rc=%d errno=%d",
               path, rc, errno);
  }
#endif
}

static void DeleteDirContents(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) {
#ifdef DSLIBRIS_DEBUG
    App *app_dbg = App::GetInstance();
    if (app_dbg)
      DBG_LOGF(app_dbg, "DeleteDirContents: opendir failed dir=%s errno=%d", dir, errno);
#endif
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    RemoveFromDir(dir, ent->d_name);
  }
  closedir(d);
}

void SettingsController::ClearAllCaches() {
  for (int i = 0; i < app_.BookCount(); i++) {
    Book *b = app_.books[i];
    if (!b)
      continue;
    b->SetPendingEpubPageCacheSave(false);
    b->SetPendingMobiPageCacheSave(false);
  }

  DeleteDirContents(paths::GetEpubCacheDir().c_str());
  DeleteDirContents(paths::GetMobiCacheDir().c_str());
  DeleteDirContents(paths::GetMobiCoverMetaCacheDir().c_str());
  DeleteDirContents(paths::GetMetaCacheDir().c_str());
  DeleteDirContents(paths::GetCoverCacheDir().c_str());

  for (int i = 0; i < app_.BookCount(); i++) {
    Book *b = app_.books[i];
    if (!b)
      continue;
    if (b->coverPixels) {
      delete[] b->coverPixels;
      b->coverPixels = nullptr;
    }
    b->coverWidth = 0;
    b->coverHeight = 0;
    b->coverAttempts = 0;
    b->metadataIndexTried = false;
  }

  app_.MarkBookLayoutDirty();

  app_.prefsButtons[PREFS_BUTTON_CLEAR_CACHE].SetLabel2(std::string("cleared!"));
  app_.PrintStatus("Caches cleared");
  app_.MarkPrefsDirty();

  if (app_.GetMode() == AppMode::Browser)
    app_.ts->MarkAllScreensDirty();
}

void SettingsController::ResetToDefaults() {
  if (!app_.prefs)
    return;
  app_.ts->SetPixelSize(12);
  app_.paraspacing = 1;
  app_.paraindent = 0;
  if (app_.orientation)
    app_.SetOrientation(false);
  app_.ts->SetColorMode(0);
  UiButtonSkin_SetColorMode(0);
  app_.prefs->time24h = true;
  app_.prefs->swapshoulder = false;
  app_.prefs->browser_view_mode = BROWSER_VIEW_GALLERY;
  app_.prefs->fixed_layout_rtl = false;
  app_.prefs->respect_publisher_font_size = false;
  app_.prefs->circle_pad_page_turn = true;
  app_.MarkBookLayoutDirty();
  app_.prefs->Write();
  for (int i = 0; i < PREFS_BUTTON_COUNT; i++)
    PrefsRefreshButton(i);
  app_.MarkPrefsDirty();
  app_.PrintStatus("Settings reset to defaults");
}

void SettingsController::PrefsHandlePress() {
  const bool is_book_ctx = app_.IsBookSettingsContext();
  Book *book = app_.GetCurrentBook();
  const int selected_button = EffectiveButtonForSlot(app_.GetPrefsSelectedIndex());

  if (selected_button == PREFS_BUTTON_ORIENTATION) {
    PrefsFlipOrientation();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_TIME24H) {
    if (CurrentBookCanGoToPage(book, is_book_ctx)) {
      if (book->GetPageCount() <= 1) {
        app_.PrintStatus("This book has only one page");
      } else {
        go_to_page_dialog_.Open();
      }
    } else {
      ToggleClockFormatSetting(app_.prefs.get());
      PrefsRefreshButton(PREFS_BUTTON_TIME24H);
      app_.MarkPrefsDirty();
    }
    return;
  }

  if (selected_button == PREFS_BUTTON_COLORMODE) {
    CycleColorMode(app_.ts.get(), &app_);
    PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
    app_.prefs->Write();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_LIBRARY_VIEW) {
    if (CurrentBookUsesLineWrapFixSlot(book, is_book_ctx)) {
      ToggleCurrentBookMobiLineWrapFix();
    } else if (CurrentBookUsesReadingDirectionSlot(book, is_book_ctx)) {
      ToggleFixedLayoutReadingDirection(app_.prefs.get());
      if (book) {
        book_renderer::ResetFixedLayoutViewportForNavigation(book);
        app_.RequestStatusRedraw();
      }
      PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
      app_.MarkPrefsDirty();
    } else {
      ToggleBrowserViewSetting(&app_);
      PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
      app_.MarkPrefsDirty();
    }
    return;
  }

  if (selected_button == PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN) {
    ToggleCirclePadPageTurnSetting(app_.prefs.get());
    PrefsRefreshButton(PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN);
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_INDEX) {
    Book *selected = app_.GetSelectedBook();
    DBG_LOGF(
        (&app_),
        "PREFS index press from_book=%d cur=%p sel=%p cur_fmt=%d sel_fmt=%d cur_ch=%u sel_ch=%u",
        is_book_ctx ? 1 : 0, (void *)book, (void *)selected,
        book ? (int)book->format : -1, selected ? (int)selected->format : -1,
        book ? (unsigned)book->GetChapters().size() : 0u,
        selected ? (unsigned)selected->GetChapters().size() : 0u);
    DBG_LOG((&app_), "PREFS index eval begin");
    const bool can_open_current = CanOpenBookIndexInCurrentContext(book, is_book_ctx);
    const bool can_open_selected = CanOpenSelectedBookIndex(selected);
    DBG_LOGF((&app_), "PREFS index eval current=%d selected=%d",
             can_open_current ? 1 : 0, can_open_selected ? 1 : 0);
    if (can_open_current) {
      DBG_LOG((&app_), "PREFS index action=open current");
      app_.ShowChaptersView();
      DBG_LOG((&app_), "PREFS index action=open current done");
    } else if (can_open_selected) {
      DBG_LOG((&app_), "PREFS index action=open selected");
      app_.OpenBook();
      DBG_LOG((&app_), "PREFS index action=open selected done");
    } else {
      DBG_LOG((&app_), "PREFS index action=unavailable");
      app_.PrintStatus("Index unavailable for this book");
      PrefsRefreshButton(PREFS_BUTTON_INDEX);
      app_.MarkPrefsDirty();
    }
    DBG_LOG((&app_), "PREFS index eval end");
    return;
  }

  if (selected_button == PREFS_BUTTON_BOOKMARKS) {
    if (is_book_ctx && book && !book->SupportsBookmarks()) {
      app_.PrintStatus("Bookmarks unavailable for PDF");
    } else if (is_book_ctx && book) {
      app_.ShowBookmarksView();
    } else if (!is_book_ctx && app_.GetSelectedBook() &&
               !app_.GetSelectedBook()->IsBrowserFolder()) {
      app_.OpenBook();
    }
    return;
  }

  if (selected_button == PREFS_BUTTON_FONT_CONFIG) {
    app_.ShowFontView(AppMode::PrefsFont);
    return;
  }

  if (selected_button == PREFS_BUTTON_PUBLISHER_FONTSIZE) {
    app_.prefs->respect_publisher_font_size = !app_.prefs->respect_publisher_font_size;
    PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_FONTSIZE);
    app_.MarkBookLayoutDirty();
    app_.prefs->Write();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_RESET_DEFAULTS) {
    ResetToDefaults();
    return;
  }

  if (selected_button == PREFS_BUTTON_CLEAR_CACHE) {
    ClearAllCaches();
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
