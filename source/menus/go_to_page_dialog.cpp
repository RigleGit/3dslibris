/*
    3dslibris - go_to_page_dialog.cpp
    New 3DS menu module by Rigle.

    Summary:
    - Implements GoToPageDialog: slider overlay for jumping to a page.
    - Extracted from settings/app_prefs.cpp to isolate the popup concern.
*/

#include "menus/go_to_page_dialog.h"

#include <3ds.h>
#include <stdio.h>

#include "app/app.h"
#include "book/book.h"
#include "book/book_renderer.h"
#include "shared/color_utils.h"
#include "ui/text.h"
#include "ui/theme_colors.h"
#include "settings/go_to_page_slider_utils.h"

namespace {

struct GoToPagePopupLayout {
  int box_x, box_y, box_w, box_h;
  int slider_x, slider_y, slider_w, slider_h;
  int cancel_x, cancel_y, cancel_w, cancel_h;
  int go_x, go_y, go_w, go_h;
};

static GoToPagePopupLayout BuildLayout() {
  GoToPagePopupLayout l;
  l.box_x = 18;
  l.box_y = 92;
  l.box_w = 204;
  l.box_h = 122;
  l.slider_x = l.box_x + 16;
  l.slider_y = l.box_y + 58;
  l.slider_w = l.box_w - 32;
  l.slider_h = 12;
  l.cancel_x = l.box_x + 16;
  l.cancel_y = l.box_y + 82;
  l.cancel_w = 72;
  l.cancel_h = 24;
  l.go_x = l.box_x + l.box_w - 88;
  l.go_y = l.cancel_y;
  l.go_w = 72;
  l.go_h = 24;
  return l;
}

static bool PointInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

} // namespace

GoToPageDialog::GoToPageDialog(App &app)
    : app_(app), open_(false), target_page_(0) {}

void GoToPageDialog::Open() {
  Book *book = app_.GetCurrentBook();
  if (!book || book->GetPageCount() <= 0)
    return;
  open_ = true;
  target_page_ =
      settings::ClampGoToPageTarget(book->GetPosition(), book->GetPageCount());
  app_.MarkPrefsDirty();
}

void GoToPageDialog::Close() {
  open_ = false;
  app_.MarkPrefsDirty();
}

bool GoToPageDialog::IsOpen() const {
  return open_;
}

void GoToPageDialog::AdjustTarget(int delta) {
  Book *book = app_.GetCurrentBook();
  if (!book || book->GetPageCount() <= 0)
    return;
  target_page_ = settings::ClampGoToPageTarget(
      target_page_ + delta, book->GetPageCount());
  app_.MarkPrefsDirty();
}

bool GoToPageDialog::Confirm() {
  Book *book = app_.GetCurrentBook();
  if (!book || book->GetPageCount() <= 0)
    return false;

  const int target_page =
      settings::ClampGoToPageTarget(target_page_, book->GetPageCount());

  if (book->IsFixedLayout())
    book_renderer::CancelFixedLayoutDeferredWork(book);
  book->SetPosition(target_page);
  if (book->IsFixedLayout())
    book_renderer::ResetFixedLayoutViewportForNavigation(book);

  app_.ShowCurrentBookView();
  book_renderer::DrawCurrentView(book, app_.ts.get());
  app_.SetPdfDeferredReadyAtMs(
      (book->IsFixedLayout() &&
       book_renderer::HasPendingFixedLayoutDeferredWork(book))
          ? (osGetTime() +
             book_renderer::GetFixedLayoutDeferredDelayMs(book))
          : 0);
  app_.RequestStatusRedraw();
  open_ = false;
  return true;
}

void GoToPageDialog::Draw() {
  Book *book = app_.GetCurrentBook();
  if (!app_.ts || !book || book->GetPageCount() <= 0 || !open_)
    return;

  const GoToPagePopupLayout layout = BuildLayout();
  const int page_count = book->GetPageCount();
  const int target_page =
      settings::ClampGoToPageTarget(target_page_, page_count);
  const int fill_w = settings::SliderPageIndexToFillWidth(
      target_page, page_count, layout.slider_w);
  const int color_mode = app_.ts->GetColorMode();
  const ThemePalette &palette = GetThemePalette(color_mode);
  const u16 box_bg = RGB565FromU8(
      (palette.bgTopR + palette.btnFillTopR) * 0.5f,
      (palette.bgTopG + palette.btnFillTopG) * 0.5f,
      (palette.bgTopB + palette.btnFillTopB) * 0.5f);
  const u16 box_border = RGB565FromU8(
      palette.btnBorderOuterR, palette.btnBorderOuterG, palette.btnBorderOuterB);
  const u16 slider_bg = RGB565FromU8(
      palette.btnFillBotR, palette.btnFillBotG, palette.btnFillBotB);
  const u16 slider_fill = RGB565FromU8(
      palette.iconR, palette.iconG, palette.iconB);
  const u16 button_bg = RGB565FromU8(
      palette.btnFillTopR, palette.btnFillTopG, palette.btnFillTopB);
  const u16 text_color = palette.textFgColor;

  char page_msg[48];
  snprintf(page_msg, sizeof(page_msg), "Pg %d / %d", target_page + 1, page_count);

  Text *ts = app_.ts.get();
  ts->FillRect((u16)layout.box_x, (u16)layout.box_y,
               (u16)(layout.box_x + layout.box_w),
               (u16)(layout.box_y + layout.box_h), box_bg);
  ts->DrawRect((u16)layout.box_x, (u16)layout.box_y,
               (u16)(layout.box_x + layout.box_w),
               (u16)(layout.box_y + layout.box_h), box_border);

  ts->SetTextColorOverride(text_color);
  ts->SetPen((u16)(layout.box_x + 16), (u16)(layout.box_y + 20));
  ts->PrintString("go to page");
  ts->SetPen((u16)(layout.box_x + 16), (u16)(layout.box_y + 40));
  ts->PrintString(page_msg);

  ts->FillRect((u16)layout.slider_x, (u16)layout.slider_y,
               (u16)(layout.slider_x + layout.slider_w),
               (u16)(layout.slider_y + layout.slider_h), slider_bg);
  ts->DrawRect((u16)layout.slider_x, (u16)layout.slider_y,
               (u16)(layout.slider_x + layout.slider_w),
               (u16)(layout.slider_y + layout.slider_h), box_border);
  if (fill_w > 0) {
    ts->FillRect((u16)layout.slider_x, (u16)layout.slider_y,
                 (u16)(layout.slider_x + fill_w),
                 (u16)(layout.slider_y + layout.slider_h), slider_fill);
  }

  const int knob_half = 4;
  const int knob_center_x = layout.slider_x + fill_w;
  ts->FillRect((u16)(knob_center_x - knob_half),
               (u16)(layout.slider_y - 3),
               (u16)(knob_center_x + knob_half + 1),
               (u16)(layout.slider_y + layout.slider_h + 4), text_color);

  ts->FillRect((u16)layout.cancel_x, (u16)layout.cancel_y,
               (u16)(layout.cancel_x + layout.cancel_w),
               (u16)(layout.cancel_y + layout.cancel_h), button_bg);
  ts->DrawRect((u16)layout.cancel_x, (u16)layout.cancel_y,
               (u16)(layout.cancel_x + layout.cancel_w),
               (u16)(layout.cancel_y + layout.cancel_h), box_border);
  ts->FillRect((u16)layout.go_x, (u16)layout.go_y,
               (u16)(layout.go_x + layout.go_w),
               (u16)(layout.go_y + layout.go_h), button_bg);
  ts->DrawRect((u16)layout.go_x, (u16)layout.go_y,
               (u16)(layout.go_x + layout.go_w),
               (u16)(layout.go_y + layout.go_h), box_border);

  ts->SetPen((u16)(layout.cancel_x + 14), (u16)(layout.cancel_y + 16));
  ts->PrintString("cancel");
  ts->SetPen((u16)(layout.go_x + 24), (u16)(layout.go_y + 16));
  ts->PrintString("go");
  ts->ClearTextColorOverride();
}

void GoToPageDialog::HandleTouch(bool touch_down) {
  Book *book = app_.GetCurrentBook();
  if (!book || book->GetPageCount() <= 0 || !open_)
    return;

  const touchPosition coord = app_.TouchRead();
  const GoToPagePopupLayout layout = BuildLayout();
  if (PointInRect(coord.px, coord.py, layout.slider_x - 4, layout.slider_y - 8,
                  layout.slider_w + 8, layout.slider_h + 16)) {
    target_page_ = settings::SliderTouchXToPageIndex(
        coord.px, layout.slider_x, layout.slider_w, book->GetPageCount());
    app_.MarkPrefsDirty();
    return;
  }

  if (!touch_down)
    return;

  if (PointInRect(coord.px, coord.py, layout.cancel_x, layout.cancel_y,
                  layout.cancel_w, layout.cancel_h)) {
    Close();
    return;
  }
  if (PointInRect(coord.px, coord.py, layout.go_x, layout.go_y, layout.go_w,
                  layout.go_h)) {
    Confirm();
  }
}
