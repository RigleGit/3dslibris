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
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <3ds.h>

#include "app/settings_controller.h"
#include "book/book.h"
#include "book/book_renderer.h"
#include "library/browser_view_utils.h"
#include "ui/button.h"
#include "ui/ui_button_skin.h"
#include "shared/color_utils.h"
#include "shared/debug_log.h"
#include "shared/orientation_utils.h"
#include "parse.h"
#include "shared/path_constants.h"
#include "settings/prefs.h"
#include "settings/prefs_action_utils.h"
#include "settings/prefs_button_context_utils.h"
#include "settings/cache_cleanup_utils.h"
#include "settings/prefs_input_utils.h"
#include "settings/prefs_style_value_utils.h"
#include "ui/text.h"
#include "ui/screen_layout_constants.h"
#include "ui/text_limits.h"

static const int PREFS_LIBRARY_BTN_X = 130;
static const int PREFS_LIBRARY_BTN_Y = 286;
static const int PREFS_LIBRARY_BTN_W = 104;
static const int PREFS_LIBRARY_BTN_H = 26;


static const int PREFS_ROW_X = 5;
static const int PREFS_ROW_W = 230;
static const u32 kGoToPageCoarseStep = 10;
static const int kLineSpacingMaxPx = 16;

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
  prefs->time24h = settings::ToggleSetting(prefs->time24h);
  prefs->RequestWrite();
}

static void ToggleReopenLastBookSetting(App *app) {
  if (!app)
    return;
  app->reopen = settings::ToggleSetting(app->reopen);
  if (app->prefs)
    app->prefs->RequestWrite();
}

static void CycleColorMode(Text *ts, App *app) {
  if (!ts)
    return;
  int mode = ts->GetColorMode();
  const int next = settings::NextCyclicSetting(mode, 6);
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
  app->prefs->RequestWrite();
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

static bool CurrentBookUsesTextLayoutSettings(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->UsesTextLayoutSettings();
}

static bool CurrentBookCanGoToPage(Book *book, bool is_book_ctx) {
  return is_book_ctx && book && book->GetPageCount() > 0;
}

static bool CurrentBookHasExtraPrefsPage(Book *book, bool is_book_ctx) {
  return is_book_ctx && book &&
         (book->UsesTextLayoutSettings() || book->IsFixedLayout());
}

static void ToggleFixedLayoutReadingDirection(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->fixed_layout_rtl = settings::ToggleSetting(prefs->fixed_layout_rtl);
  prefs->RequestWrite();
}

static void ToggleCirclePadPageTurnSetting(Prefs *prefs) {
  if (!prefs)
    return;
  prefs->circle_pad_page_turn =
      settings::ToggleSetting(prefs->circle_pad_page_turn);
  prefs->RequestWrite();
}

static void CycleLibrarySortSetting(App *app) {
  if (!app || !app->prefs)
    return;
  const int next = settings::NextCyclicSetting(
      static_cast<int>(app->prefs->library_sort_mode), LIBRARY_SORT_COUNT);
  app->prefs->library_sort_mode = static_cast<LibrarySortMode>(next);
  app->prefs->RequestWrite();
  app->ReSortLibraryBooks();
  app->ResetBrowserMarquee();
  app->MarkBrowserDirty();
}

static settings::StyleValueContext StyleValueForBook(
    bool is_book_ctx, Book *book, int global_value, int override_value) {
  settings::StyleValueContext context;
  context.from_book = is_book_ctx && book;
  context.uses_text_layout = !book || book->UsesTextLayoutSettings();
  context.global_value = global_value;
  context.override_value = override_value;
  return context;
}

static void TogglePublisherTextIndentSetting(App *app, Book *book, bool is_book_ctx) {
  if (!app)
    return;
  if (is_book_ctx && book && book->UsesTextLayoutSettings()) {
    book->SetStylePublisherTextIndentOverride(
        settings::NextTriStateOverride(
            book->GetStylePublisherTextIndentOverride()));
    app->MarkBookLayoutDirty();
  } else {
    app->publisher_text_indent =
        settings::ToggleSetting(app->publisher_text_indent);
    app->MarkBookLayoutDirty();
  }
  if (app->prefs)
    app->prefs->RequestWrite();
}

static void TogglePublisherBlockMarginsSetting(App *app, Book *book, bool is_book_ctx) {
  if (!app)
    return;
  if (is_book_ctx && book && book->UsesTextLayoutSettings()) {
    book->SetStylePublisherBlockMarginsOverride(
        settings::NextTriStateOverride(
            book->GetStylePublisherBlockMarginsOverride()));
    app->MarkBookLayoutDirty();
  } else {
    app->publisher_block_margins =
        settings::ToggleSetting(app->publisher_block_margins);
    app->MarkBookLayoutDirty();
  }
  if (app->prefs)
    app->prefs->RequestWrite();
}

static void TogglePublisherHorizontalMarginsSetting(App *app, Book *book,
                                                    bool is_book_ctx) {
  if (!app)
    return;
  if (is_book_ctx && book && book->UsesTextLayoutSettings()) {
    book->SetStylePublisherHorizontalMarginsOverride(
        settings::NextTriStateOverride(
            book->GetStylePublisherHorizontalMarginsOverride()));
    app->MarkBookLayoutDirty();
  } else {
    app->publisher_horizontal_margins =
        settings::ToggleSetting(app->publisher_horizontal_margins);
    app->MarkBookLayoutDirty();
  }
  if (app->prefs)
    app->prefs->RequestWrite();
}

SettingsController::SettingsController(App &app)
    : app_(app), go_to_page_dialog_(app), prefs_general_page_(0) {}

int SettingsController::EffectiveVisibleCount() const {
  const bool is_book_ctx = app_.IsBookSettingsContext();
  settings::PrefsPageContext context;
  context.from_book = is_book_ctx;
  context.page = prefs_general_page_;
  context.fixed_layout =
      app_.GetCurrentBook() && app_.GetCurrentBook()->IsFixedLayout();
  context.include_line_wrap_fix =
      CurrentBookUsesLineWrapFixSlot(app_.GetCurrentBook(), is_book_ctx);
  return (int)settings::PrefsPageButtonCount(context);
}

int SettingsController::EffectiveButtonForSlot(int slot) const {
  const bool is_book_ctx = app_.IsBookSettingsContext();
  settings::PrefsPageContext context;
  context.from_book = is_book_ctx;
  context.page = prefs_general_page_;
  context.fixed_layout =
      app_.GetCurrentBook() && app_.GetCurrentBook()->IsFixedLayout();
  context.include_line_wrap_fix =
      CurrentBookUsesLineWrapFixSlot(app_.GetCurrentBook(), is_book_ctx);
  return settings::PrefsPageButtonForSlot(context, (u8)slot);
}

void SettingsController::GoToPrefsPage(int page) {
  prefs_general_page_ = page;
  app_.SetPrefsSelectedIndex(0);
  app_.MarkPrefsDirty();
}

void SettingsController::ShowSettingsView(bool from_book) {
  app_.ApplyRenderOrientation(app_.portrait_orientation);
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
  app_.prefs->RequestWrite();
  app_.MarkPrefsDirty();
}


u8 SettingsController::PrefsVisibleButtonCount() const {
  return (u8)EffectiveVisibleCount();
}

void SettingsController::PrefsInit() {
  const std::vector<std::string> labels{
      "style customization",
      "font configuration", "font size",    "extra line spacing",
      "extra paragraph spacing", "reading orientation", "handedness",
      "clock format",
      "time remaining", "reopen last book", "color mode", "library view",
      "circle pad pages", "library sort", "book information", "index", "bookmarks",
      "reset settings",
      "clear cache",        "publisher indent", "publisher spacing",
      "publisher sides"};

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

  PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
  PrefsRefreshButton(PREFS_BUTTON_LINE_SPACING);
  PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
  PrefsRefreshButton(PREFS_BUTTON_STYLE_CUSTOMIZATION);
  PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
  PrefsRefreshButton(PREFS_BUTTON_TIME24H);
  PrefsRefreshButton(PREFS_BUTTON_TIME_REMAINING);
  PrefsRefreshButton(PREFS_BUTTON_REOPEN_LAST_BOOK);
  PrefsRefreshButton(PREFS_BUTTON_COLORMODE);
  PrefsRefreshButton(PREFS_BUTTON_LIBRARY_VIEW);
  PrefsRefreshButton(PREFS_BUTTON_BOOK_INFO);
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);
  PrefsRefreshButton(PREFS_BUTTON_LIBRARY_SORT);
  PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_TEXT_INDENT);
  PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS);
  PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS);

  for (int slot = 0; slot < visibleCount; slot++) {
    const int button_id = EffectiveButtonForSlot(slot);
    const int row_pitch = settings::PrefsRowPitch(visibleCount);
    app_.prefsButtons[button_id].Resize(230, row_pitch - 2);
    app_.prefsButtons[button_id].Move(5, slot * row_pitch);
    app_.prefsButtons[button_id].Draw(ts->screenright,
                                      slot == app_.GetPrefsSelectedIndex());
  }

  if (go_to_page_dialog_.IsOpen())
    go_to_page_dialog_.Draw();

  const bool book_ctx = app_.IsBookSettingsContext();
  const bool has_submenu =
      !book_ctx || CurrentBookHasExtraPrefsPage(app_.GetCurrentBook(), book_ctx);
  const bool general_paged = !book_ctx;
  if (book_ctx) {
    SyncLibraryButtonLayout(&app_.buttonprefs, false, true);
    app_.buttonprefs.Draw(ts->screenright);
    button_prefs_library_.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
    button_prefs_library_.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
    button_prefs_library_.Draw(ts->screenright);
    if (has_submenu) {
      button_prefs_page_nav_.Label(prefs_general_page_ == 0 ? "next" : "prev");
      button_prefs_page_nav_.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
      button_prefs_page_nav_.Draw(ts->screenright);
    }
  } else if (general_paged) {
    SyncLibraryButtonLayout(&app_.buttonprefs, true, false);
    app_.buttonprefs.Draw(ts->screenright);
    if (prefs_general_page_ == 0) {
      button_prefs_page_nav_.Label("next");
      button_prefs_page_nav_.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
      button_prefs_page_nav_.Draw(ts->screenright);
    } else if (prefs_general_page_ == 2) {
      button_prefs_page_nav_.Label("prev");
      button_prefs_page_nav_.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
      button_prefs_page_nav_.Draw(ts->screenright);
    } else {
      button_prefs_page_nav_.Label("back");
      button_prefs_page_nav_.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
      button_prefs_page_nav_.Draw(ts->screenright);
    }
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

void SettingsController::PrefsHandleEvent(const FrameInput &input) {
  const u32 keys = input.keys_down;
  const u32 held = input.keys_held;
  const u32 left_keys = app_.key.left | app_.key.dleft;
  const u32 right_keys = app_.key.right | app_.key.dright;
  const u32 up_keys = app_.key.up | app_.key.dup;
  const u32 down_keys = app_.key.down | app_.key.ddown;
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
  const bool book_ctx = app_.IsBookSettingsContext();
  const bool has_submenu =
      !book_ctx || CurrentBookHasExtraPrefsPage(app_.GetCurrentBook(), book_ctx);

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
    if (keys & left_keys) {
      go_to_page_dialog_.AdjustTarget(-1);
    } else if (keys & right_keys) {
      go_to_page_dialog_.AdjustTarget(1);
    } else if (keys & (up_keys | app_.key.l)) {
      go_to_page_dialog_.AdjustTarget(-(int)kGoToPageCoarseStep);
    } else if (keys & (down_keys | app_.key.r)) {
      go_to_page_dialog_.AdjustTarget((int)kGoToPageCoarseStep);
    }
    if ((keys & KEY_TOUCH) || (held & KEY_TOUCH))
      go_to_page_dialog_.HandleTouch(input, (keys & KEY_TOUCH) != 0);
    if (prefs_input_utils::ShouldRedrawPrefsAfterOverlayInput(
            app_.IsPrefsDirty(), app_.GetMode() == AppMode::Prefs))
      PrefsDraw();
    return;
  }

  if (keys & KEY_A) {
    PrefsHandlePress();
    if (app_.GetMode() != AppMode::Prefs)
      return;
  } else if (book_ctx && (keys & KEY_START)) {
    app_.ShowLibraryView();
  } else if (prefs_input_utils::ShouldReturnFromPrefs(
                 keys, book_ctx, KEY_B, KEY_SELECT, KEY_Y, KEY_START)) {
    app_.ReturnFromPrefs();
  } else if (keys & (left_keys | app_.key.l)) {
    if (app_.GetPrefsSelectedIndex() > 0) {
      app_.SetPrefsSelectedIndex(app_.GetPrefsSelectedIndex() - 1);
      app_.MarkPrefsDirty();
    } else if (!book_ctx && prefs_general_page_ == 2) {
      GoToPrefsPage(0);
    } else if (prefs_general_page_ == 1 && has_submenu) {
      GoToPrefsPage(0);
    }
  } else if (keys & (right_keys | app_.key.r)) {
    if (app_.GetPrefsSelectedIndex() < visibleCount - 1) {
      app_.SetPrefsSelectedIndex(app_.GetPrefsSelectedIndex() + 1);
      app_.MarkPrefsDirty();
    } else if (!book_ctx && prefs_general_page_ == 0) {
      GoToPrefsPage(2);
    } else if (book_ctx && prefs_general_page_ == 0 && has_submenu) {
      GoToPrefsPage(1);
    }
  } else if (selected_button == PREFS_BUTTON_FONTSIZE &&
             (keys & up_keys)) {
    PrefsDecreasePixelSize();
  } else if (selected_button == PREFS_BUTTON_FONTSIZE &&
             (keys & down_keys)) {
    PrefsIncreasePixelSize();
  } else if (selected_button == PREFS_BUTTON_LINE_SPACING &&
             (keys & up_keys)) {
    PrefsDecreaseLineSpacing();
  } else if (selected_button == PREFS_BUTTON_LINE_SPACING &&
             (keys & down_keys)) {
    PrefsIncreaseLineSpacing();
  } else if (selected_button == PREFS_BUTTON_PARASPACING &&
             (keys & up_keys)) {
    PrefsDecreaseParaspacing();
  } else if (selected_button == PREFS_BUTTON_PARASPACING &&
             (keys & down_keys)) {
    PrefsIncreaseParaspacing();
  } else if (keys & KEY_TOUCH) {
    PrefsHandleTouch(input);
  }
}

void SettingsController::PrefsHandleTouch(const FrameInput &input) {
  const AppMode mode_before_touch = app_.GetMode();
  touchPosition coord = app_.MapTouch(input);
  const int footerX = (int)coord.px;
  const int footerY = (int)coord.py;

  const bool book_ctx_touch = app_.IsBookSettingsContext();
  const bool has_submenu =
      !book_ctx_touch ||
      CurrentBookHasExtraPrefsPage(app_.GetCurrentBook(), book_ctx_touch);
  SyncLibraryButtonLayout(&app_.buttonprefs, !book_ctx_touch, book_ctx_touch);
  if (book_ctx_touch) {
    button_prefs_library_.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
    button_prefs_library_.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
    if (has_submenu)
      button_prefs_page_nav_.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
  }
  if (!book_ctx_touch && prefs_general_page_ == 2) {
    button_prefs_page_nav_.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
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
    return;
  }

  if (book_ctx_touch && has_submenu &&
      enclosesWithSlack(button_prefs_page_nav_, footerX, footerY)) {
    GoToPrefsPage(prefs_general_page_ == 0 ? 1 : 0);
    return;
  }
  if (!book_ctx_touch &&
      enclosesWithSlack(button_prefs_page_nav_, footerX, footerY)) {
    GoToPrefsPage(prefs_general_page_ == 0 ? 2 : 0);
    return;
  }

  u8 visibleCount = NormalizeVisibleCount(PrefsVisibleButtonCount());
  int selected_index = app_.GetPrefsSelectedIndex();
  ClampSelectedIndex(&selected_index, visibleCount);
  app_.SetPrefsSelectedIndex(selected_index);
  for (u8 i = 0; i < visibleCount; i++) {
    const int button_id = EffectiveButtonForSlot(i);
    if (enclosesWithSlack(app_.prefsButtons[button_id], (int)coord.px,
                          (int)coord.py)) {
      if (i != app_.GetPrefsSelectedIndex())
        app_.SetPrefsSelectedIndex(i);

      if (button_id == PREFS_BUTTON_FONTSIZE) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreasePixelSize();
        } else {
          PrefsDecreasePixelSize();
        }
      } else if (button_id == PREFS_BUTTON_LINE_SPACING) {
        int centerX = PREFS_ROW_X + PREFS_ROW_W / 2;
        if (coord.px >= centerX) {
          PrefsIncreaseLineSpacing();
        } else {
          PrefsDecreaseLineSpacing();
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
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    const int value = settings::EffectiveStyleValue(StyleValueForBook(
        true, book, app_.reader_font_size,
        book->GetStyleFontSizeOverride()));
    if (value < kTextPixelSizeMax) {
      book->SetStyleFontSizeOverride(value + 1);
      app_.ts->SetPixelSize((u8)(value + 1));
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
      app_.prefs->RequestWrite();
    }
  } else if (app_.reader_font_size < kTextPixelSizeMax) {
    app_.reader_font_size++;
    app_.ts->SetPixelSize((u8)app_.reader_font_size);
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsDecreasePixelSize() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    const int value = settings::EffectiveStyleValue(StyleValueForBook(
        true, book, app_.reader_font_size,
        book->GetStyleFontSizeOverride()));
    if (value > kTextPixelSizeMin) {
      book->SetStyleFontSizeOverride(value - 1);
      app_.ts->SetPixelSize((u8)(value - 1));
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
      app_.prefs->RequestWrite();
    }
  } else if (app_.reader_font_size > kTextPixelSizeMin) {
    app_.reader_font_size--;
    app_.ts->SetPixelSize((u8)app_.reader_font_size);
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_FONTSIZE);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsIncreaseLineSpacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    const int value = settings::EffectiveStyleValue(StyleValueForBook(
        true, book, app_.reader_line_spacing,
        book->GetStyleLineSpacingOverride()));
    if (value < kLineSpacingMaxPx) {
      book->SetStyleLineSpacingOverride(value + 1);
      app_.ts->linespacing = value + 1;
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_LINE_SPACING);
      app_.prefs->RequestWrite();
    }
  } else if (app_.reader_line_spacing < kLineSpacingMaxPx) {
    app_.reader_line_spacing++;
    app_.ts->linespacing = app_.reader_line_spacing;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_LINE_SPACING);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsDecreaseLineSpacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    const int value = settings::EffectiveStyleValue(StyleValueForBook(
        true, book, app_.reader_line_spacing,
        book->GetStyleLineSpacingOverride()));
    if (value > 0) {
      book->SetStyleLineSpacingOverride(value - 1);
      app_.ts->linespacing = value - 1;
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_LINE_SPACING);
      app_.prefs->RequestWrite();
    }
  } else if (app_.reader_line_spacing > 0) {
    app_.reader_line_spacing--;
    app_.ts->linespacing = app_.reader_line_spacing;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_LINE_SPACING);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsIncreaseParaspacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    int value = book->GetStyleParagraphSpacingOverride();
    if (value < 0)
      value = book->GetParagraphSpacing();
    if (value < 4) {
      book->SetStyleParagraphSpacingOverride(value + 1);
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
      app_.prefs->RequestWrite();
    }
  } else if (app_.paraspacing < 4) {
    app_.paraspacing++;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsDecreaseParaspacing() {
  if (app_.IsBookSettingsContext() &&
      !CurrentBookUsesTextLayoutSettings(app_.GetCurrentBook(), true))
    return;
  Book *book = app_.GetCurrentBook();
  if (app_.IsBookSettingsContext() && book) {
    int value = book->GetStyleParagraphSpacingOverride();
    if (value < 0)
      value = book->GetParagraphSpacing();
    if (value > 0) {
      book->SetStyleParagraphSpacingOverride(value - 1);
      app_.MarkBookLayoutDirty();
      PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
      app_.prefs->RequestWrite();
    }
  } else if (app_.paraspacing > 0) {
    app_.paraspacing--;
    app_.MarkBookLayoutDirty();
    PrefsRefreshButton(PREFS_BUTTON_PARASPACING);
    app_.prefs->RequestWrite();
  }
}

void SettingsController::PrefsFlipOrientation() {
  const u8 next_orientation = app_.landscape
                                  ? app_.portrait_orientation
                                  : orientation_utils::ORIENT_LANDSCAPE;
  app_.SetOrientation(next_orientation);
  app_.MarkBookLayoutDirty();
  PrefsRefreshButton(PREFS_BUTTON_ORIENTATION);
  app_.prefs->RequestWrite();
  if (app_.GetMode() == AppMode::Prefs)
    PrefsDraw();
}

void SettingsController::PrefsToggleHandedness() {
  const u8 next = orientation_utils::IsTurnedRight(app_.portrait_orientation)
                      ? orientation_utils::ORIENT_TURNED_LEFT
                      : orientation_utils::ORIENT_TURNED_RIGHT;
  app_.SetHandedness(next);
  PrefsRefreshButton(PREFS_BUTTON_HANDEDNESS);
  app_.prefs->RequestWrite();
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
  case PREFS_BUTTON_STYLE_CUSTOMIZATION:
    app_.prefsButtons[PREFS_BUTTON_STYLE_CUSTOMIZATION].SetLabel1(
        is_book_ctx ? std::string("book style customization")
                    : std::string("global style customization"));
    if (is_book_ctx && book && !book->UsesTextLayoutSettings()) {
      app_.prefsButtons[PREFS_BUTTON_STYLE_CUSTOMIZATION].SetLabel2(
          std::string("(PDF fixed)"));
    } else {
      app_.prefsButtons[PREFS_BUTTON_STYLE_CUSTOMIZATION].SetLabel2(
          std::string(">"));
    }
    break;
  case PREFS_BUTTON_FONTSIZE:
    app_.prefsButtons[PREFS_BUTTON_FONTSIZE].SetLabel2(
        settings::FontSizeValueLabel(StyleValueForBook(
            is_book_ctx, book, app_.reader_font_size,
            book ? book->GetStyleFontSizeOverride() : -1)));
    break;
  case PREFS_BUTTON_LINE_SPACING:
    app_.prefsButtons[PREFS_BUTTON_LINE_SPACING].SetLabel1(
        std::string("extra line spacing"));
    app_.prefsButtons[PREFS_BUTTON_LINE_SPACING].SetLabel2(
        settings::LineSpacingValueLabel(StyleValueForBook(
            is_book_ctx, book, app_.reader_line_spacing,
            book ? book->GetStyleLineSpacingOverride() : -1)));
    break;
  case PREFS_BUTTON_PARASPACING:
    app_.prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel1(
        std::string("extra paragraph spacing"));
    app_.prefsButtons[PREFS_BUTTON_PARASPACING].SetLabel2(
        settings::ParagraphSpacingValueLabel(StyleValueForBook(
            is_book_ctx, book, app_.paraspacing,
            book ? book->GetStyleParagraphSpacingOverride() : -1)));
    break;
  case PREFS_BUTTON_ORIENTATION:
    app_.prefsButtons[PREFS_BUTTON_ORIENTATION].SetLabel2(
        app_.landscape ? std::string("Horizontal")
                       : std::string("Vertical"));
    break;
  case PREFS_BUTTON_HANDEDNESS:
    app_.prefsButtons[PREFS_BUTTON_HANDEDNESS].SetLabel2(
        orientation_utils::IsTurnedRight(app_.portrait_orientation)
            ? std::string("Left-handed")
            : std::string("Right-handed"));
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
  case PREFS_BUTTON_TIME_REMAINING:
    app_.prefsButtons[PREFS_BUTTON_TIME_REMAINING].SetLabel1(
        std::string("time remaining"));
    app_.prefsButtons[PREFS_BUTTON_TIME_REMAINING].SetLabel2(
        app_.prefs->show_time_remaining ? std::string("on")
                                         : std::string("off"));
    break;
  case PREFS_BUTTON_REOPEN_LAST_BOOK:
    app_.prefsButtons[PREFS_BUTTON_REOPEN_LAST_BOOK].SetLabel1(
        std::string("reopen last book"));
    app_.prefsButtons[PREFS_BUTTON_REOPEN_LAST_BOOK].SetLabel2(
        app_.reopen ? std::string("on") : std::string("off"));
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
  case PREFS_BUTTON_LIBRARY_SORT: {
    static const char *const kSortModeLabels[LIBRARY_SORT_COUNT] = {
        "by title", "by filename", "by author",
        "by file type", "by date modified", "by recently opened"};
    const int mode = static_cast<int>(app_.prefs->library_sort_mode);
    app_.prefsButtons[PREFS_BUTTON_LIBRARY_SORT].SetLabel1(
        std::string("library sort"));
    app_.prefsButtons[PREFS_BUTTON_LIBRARY_SORT].SetLabel2(
        std::string(kSortModeLabels[mode >= 0 && mode < LIBRARY_SORT_COUNT
                                        ? mode
                                        : 0]));
    break;
  }
  case PREFS_BUTTON_BOOK_INFO:
    app_.prefsButtons[PREFS_BUTTON_BOOK_INFO].SetLabel1(
        std::string("book information"));
    if (is_book_ctx && book) {
      app_.prefsButtons[PREFS_BUTTON_BOOK_INFO].SetLabel2(std::string("open >"));
    } else {
      app_.prefsButtons[PREFS_BUTTON_BOOK_INFO].SetLabel2(
          std::string("(book only)"));
    }
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
  case PREFS_BUTTON_RESET_DEFAULTS:
    app_.prefsButtons[PREFS_BUTTON_RESET_DEFAULTS].SetLabel2(std::string("restore defaults >"));
    break;
  case PREFS_BUTTON_CLEAR_CACHE:
    app_.prefsButtons[PREFS_BUTTON_CLEAR_CACHE].SetLabel2(std::string("delete all caches >"));
    break;
  case PREFS_BUTTON_PUBLISHER_TEXT_INDENT:
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_TEXT_INDENT].SetLabel1(
        std::string("publisher indent"));
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_TEXT_INDENT].SetLabel2(
        settings::PublisherSettingValueLabel(
            is_book_ctx && book, !book || book->UsesTextLayoutSettings(),
            book ? book->GetStylePublisherTextIndentOverride() : -1,
            app_.publisher_text_indent,
            book ? book->GetPublisherTextIndentEnabled()
                 : app_.publisher_text_indent));
    break;
  case PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS:
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS].SetLabel1(
        std::string("publisher spacing"));
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS].SetLabel2(
        settings::PublisherSettingValueLabel(
            is_book_ctx && book, !book || book->UsesTextLayoutSettings(),
            book ? book->GetStylePublisherBlockMarginsOverride() : -1,
            app_.publisher_block_margins,
            book ? book->GetPublisherBlockMarginsEnabled()
                 : app_.publisher_block_margins));
    break;
  case PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS:
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS].SetLabel1(
        std::string("publisher sides"));
    app_.prefsButtons[PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS].SetLabel2(
        settings::PublisherSettingValueLabel(
            is_book_ctx && book, !book || book->UsesTextLayoutSettings(),
            book ? book->GetStylePublisherHorizontalMarginsOverride() : -1,
            app_.publisher_horizontal_margins,
            book ? book->GetPublisherHorizontalMarginsEnabled()
                 : app_.publisher_horizontal_margins));
    break;
  }
  app_.MarkPrefsDirty();
}

void SettingsController::ClearAllCaches() {
  for (int i = 0; i < app_.BookCount(); i++) {
    Book *b = app_.books[i];
    if (!b)
      continue;
    b->SetPendingEpubPageCacheSave(false);
    b->SetPendingMobiPageCacheSave(false);
  }

  const std::string cache_dirs[] = {
      paths::GetEpubCacheDir(), paths::GetMobiCacheDir(),
      paths::GetMobiCoverMetaCacheDir(), paths::GetMetaCacheDir(),
      paths::GetCoverCacheDir()};
  for (size_t i = 0; i < sizeof(cache_dirs) / sizeof(cache_dirs[0]); i++) {
#ifdef DSLIBRIS_DEBUG
    const settings::CacheCleanupResult result =
        settings::DeleteCacheDirectoryContents(cache_dirs[i].c_str());
    if (!result.opened || result.failed > 0)
      DBG_LOGF(&app_, "ClearAllCaches: dir=%s opened=%d removed=%d failed=%d",
               cache_dirs[i].c_str(), result.opened ? 1 : 0, result.removed,
               result.failed);
#else
    settings::DeleteCacheDirectoryContents(cache_dirs[i].c_str());
#endif
  }

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
  app_.reader_font_size = 12;
  app_.reader_line_spacing = 0;
  app_.ts->SetPixelSize(12);
  app_.ts->linespacing = app_.reader_line_spacing;
  app_.paraspacing = 0;
  app_.paraindent = 0;
  app_.publisher_text_indent = true;
  app_.publisher_block_margins = true;
  app_.publisher_horizontal_margins = true;
  if (app_.orientation != orientation_utils::ORIENT_TURNED_LEFT)
    app_.SetOrientation(orientation_utils::ORIENT_TURNED_LEFT);
  app_.ts->SetColorMode(0);
  UiButtonSkin_SetColorMode(0);
  app_.prefs->time24h = true;
  app_.prefs->swapshoulder = false;
  app_.prefs->browser_view_mode = BROWSER_VIEW_GALLERY;
  app_.prefs->fixed_layout_rtl = false;
  app_.prefs->circle_pad_page_turn = true;
  app_.prefs->show_time_remaining = false;
  app_.reopen = true;
  app_.MarkBookLayoutDirty();
  app_.prefs->RequestWrite();
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

  if (selected_button == PREFS_BUTTON_HANDEDNESS) {
    PrefsToggleHandedness();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_STYLE_CUSTOMIZATION) {
    if (is_book_ctx && (!book || !book->UsesTextLayoutSettings())) {
      app_.PrintStatus("Style settings unavailable for fixed-layout books");
      PrefsRefreshButton(PREFS_BUTTON_STYLE_CUSTOMIZATION);
      app_.MarkPrefsDirty();
    } else {
      GoToPrefsPage(1);
    }
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
    app_.prefs->RequestWrite();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_TIME_REMAINING) {
    app_.prefs->show_time_remaining =
        settings::ToggleSetting(app_.prefs->show_time_remaining);
    PrefsRefreshButton(PREFS_BUTTON_TIME_REMAINING);
    app_.prefs->RequestWrite();
    app_.RequestStatusRedraw();
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_REOPEN_LAST_BOOK) {
    ToggleReopenLastBookSetting(&app_);
    PrefsRefreshButton(PREFS_BUTTON_REOPEN_LAST_BOOK);
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

  if (selected_button == PREFS_BUTTON_LIBRARY_SORT) {
    CycleLibrarySortSetting(&app_);
    PrefsRefreshButton(PREFS_BUTTON_LIBRARY_SORT);
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_BOOK_INFO) {
    if (is_book_ctx && book) {
      app_.ShowBookInfoView();
    } else {
      app_.PrintStatus("Book information is available while reading");
      PrefsRefreshButton(PREFS_BUTTON_BOOK_INFO);
      app_.MarkPrefsDirty();
    }
    return;
  }

  if (selected_button == PREFS_BUTTON_PUBLISHER_TEXT_INDENT) {
    TogglePublisherTextIndentSetting(&app_, book, is_book_ctx);
    PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_TEXT_INDENT);
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS) {
    TogglePublisherBlockMarginsSetting(&app_, book, is_book_ctx);
    PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS);
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS) {
    TogglePublisherHorizontalMarginsSetting(&app_, book, is_book_ctx);
    PrefsRefreshButton(PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS);
    app_.MarkPrefsDirty();
    return;
  }

  if (selected_button == PREFS_BUTTON_INDEX) {
    Book *selected = app_.GetSelectedBook();
    const bool can_open_current = CanOpenBookIndexInCurrentContext(book, is_book_ctx);
    const bool can_open_selected = CanOpenSelectedBookIndex(selected);
    if (can_open_current) {
      app_.ShowChaptersView();
    } else if (can_open_selected) {
      app_.OpenBook();
    } else {
      app_.PrintStatus("Index unavailable for this book");
      PrefsRefreshButton(PREFS_BUTTON_INDEX);
      app_.MarkPrefsDirty();
    }
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

void App::PrefsHandleEvent(const FrameInput &input) {
  settings_controller_->PrefsHandleEvent(input);
}

void App::PrefsHandleTouch(const FrameInput &input) {
  settings_controller_->PrefsHandleTouch(input);
}

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
