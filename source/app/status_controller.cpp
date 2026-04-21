/*
    3dslibris - status_controller.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Updates the in-reader status HUD for Book and Opening modes.
    - Supports a minimal fixed-layout HUD for PDF/CBZ and a progress HUD for reflowable books.
    - Avoids unnecessary redraws by caching the last displayed time/progress token and
      only repainting when the visible status changes or a redraw is forced.
*/

#include "app/status_controller.h"

#include <algorithm>
#include <time.h>

#include "app/app.h"
#include "app/status_layout_utils.h"
#include "book/book.h"
#include "shared/app_flow_utils.h"
#include "settings/prefs.h"
#include "ui/text.h"

namespace
{

  // Helper function to determine if the current book uses a fixed layout and should show a minimal HUD.
  static bool UsesFixedLayoutMinimalHud(const Book *book)
  {
    return book && (book->IsPdf() || book->IsCbz());
  }
  // TODO: Route HUD status labels like "opening" through a central UI string
  // table if the app later adds localization or more status variants.
  static const char *kOpeningStatusLabel = "opening";
} // namespace

StatusController::StatusController(App &app)
    : app_(app), last_minute_(-1), last_display_token_(-1),
      progress_lock_book_(nullptr), progress_pagecount_lock_(0),
      force_redraw_(true) {}

void StatusController::RequestStatusRedraw() { force_redraw_ = true; }

// Updates the status HUD based on the current app mode, time, and book progress. Avoids unnecessary redraws by caching the last displayed state.
void StatusController::UpdateStatus()
{
  const AppMode mode = app_.GetMode();
  if (mode != AppMode::Book && mode != AppMode::Opening)
    return;
  u16 *screen = app_.ts->GetScreen();
  time_t unixTime = time(NULL);
  // TODO: Move clock formatting into a small shared helper if status/date/time
  // rendering grows further (12h/24h logic currently lives inline here).
  struct tm *timeStruct = localtime(&unixTime);
  int minute_of_day = -1;
  if (timeStruct)
  {
    minute_of_day = timeStruct->tm_hour * 60 + timeStruct->tm_min;
  }

  Book *current_book = app_.GetCurrentBook();
  app_flow_utils::StatusSnapshot snapshot = {};
  if (mode == AppMode::Book)
  {
    snapshot = app_flow_utils::ComputeStatusSnapshot(
        {current_book, current_book ? (int)current_book->GetPosition() : 0,
         current_book ? (int)current_book->GetPageCount() : 0,
         false,
         progress_lock_book_, progress_pagecount_lock_});
    progress_lock_book_ = (Book *)snapshot.next_locked_book; // TODO: avoid C-style cast here by changing snapshot to use Book* directly.
    progress_pagecount_lock_ = snapshot.next_locked_pagecount;
  }
  else
  {
    progress_lock_book_ = nullptr;
    progress_pagecount_lock_ = 0;
    snapshot.percent_tenths = -1;
    snapshot.percent_value = 0.0f;
    snapshot.draw_page_count = 0;
    snapshot.has_progress = false;
    snapshot.next_locked_book = nullptr;
    snapshot.next_locked_pagecount = 0;
  }

  if (!force_redraw_ && minute_of_day == last_minute_ &&
      ((mode == AppMode::Book && UsesFixedLayoutMinimalHud(current_book))
           ? (current_book ? (int)current_book->GetPosition() : -1)
           : snapshot.percent_tenths) == last_display_token_)
  {
    return;
  }

  char tmsg[24];
  if (!timeStruct)
  {
    snprintf(tmsg, sizeof(tmsg), "--:--");
  }
  else if (app_.prefs->time24h)
  {
    snprintf(tmsg, sizeof(tmsg), "%02d:%02d", timeStruct->tm_hour,
             timeStruct->tm_min);
  }
  else
  {
    int h = timeStruct->tm_hour % 12;
    if (h == 0)
      h = 12;
    snprintf(tmsg, sizeof(tmsg), "%02d:%02d %s", h, timeStruct->tm_min,
             timeStruct->tm_hour >= 12 ? "PM" : "AM");
  }

  int style = app_.ts->GetStyle();
  app_.ts->SetStyle(TEXT_STYLE_BROWSER); // smaller, readable font

  if (mode == AppMode::Book &&
      UsesFixedLayoutMinimalHud(current_book))
  {
    const status_layout_utils::FixedLayoutBottomHudLayout hud_layout =
        status_layout_utils::ComputeFixedLayoutBottomHudLayout(
            320, app_.ts->GetHeight());
    const int time_width = app_.ts->GetStringWidth(tmsg, TEXT_STYLE_BROWSER);
    const int time_x = std::max(0, 240 - hud_layout.right_margin - time_width);

    app_.ts->SetScreen(app_.ts->screenright);
    app_.ts->ClearRect((u16)std::max(0, time_x - 4),
                       (u16)hud_layout.time_clear_top, 240,
                       (u16)hud_layout.time_clear_bottom);
    app_.ts->SetPen(time_x, hud_layout.time_y);
    app_.ts->PrintString(tmsg);

    if (current_book && current_book->GetPageCount() > 0)
    {
      char page_msg[32];
      snprintf(page_msg, sizeof(page_msg), "%d/%d",
               (int)current_book->GetPosition() + 1,
               (int)current_book->GetPageCount());
      const int page_width =
          app_.ts->GetStringWidth(page_msg, TEXT_STYLE_BROWSER);
      const int page_x = std::max(0, 240 - hud_layout.right_margin - page_width);
      app_.ts->ClearRect((u16)std::max(0, page_x - 4),
                         (u16)hud_layout.page_clear_top, 240,
                         (u16)hud_layout.page_clear_bottom);
      app_.ts->SetPen(page_x, hud_layout.page_y);
      app_.ts->PrintString(page_msg);
    }
  }
  else
  {
    app_.ts->SetScreen(app_.ts->screenleft);
    int savedBottomMargin = app_.ts->margin.bottom;
    app_.ts->margin.bottom = 0;
    u16 fgColor = app_.ts->GetFgColor();

    const status_layout_utils::BookStatusHudLayout hud_layout =
        status_layout_utils::ComputeBookStatusHudLayout(
            400, app_.ts->GetHeight(), savedBottomMargin);
    app_.ts->ClearRect(0, (u16)hud_layout.clear_top, 240,
                       (u16)hud_layout.clear_bottom);
    const int textY = hud_layout.text_y;
    app_.ts->SetPen(8, textY);
    app_.ts->PrintString(tmsg);
    int clockWidth = app_.ts->GetStringWidth(tmsg, TEXT_STYLE_BROWSER);

    int pX = 232;
    if (mode == AppMode::Opening)
    {
      int pw = app_.ts->GetStringWidth(kOpeningStatusLabel, TEXT_STYLE_BROWSER);
      pX = 232 - pw;
      app_.ts->SetPen(pX, textY);
      app_.ts->PrintString(kOpeningStatusLabel);
    }
    else if (snapshot.has_progress)
    {
      char pmsg[32];
      snprintf(pmsg, sizeof(pmsg), "%.1f%%", snapshot.percent_value);
      int pw = app_.ts->GetStringWidth(pmsg, TEXT_STYLE_BROWSER);
      pX = 232 - pw;
      app_.ts->SetPen(pX, textY);
      app_.ts->PrintString(pmsg);

      int barStart = 8 + clockWidth + 12;
      int barEnd = pX - 12;
      if (barEnd > barStart + 10)
      {
        int barY = hud_layout.progress_bar_y;
        int barHeight = hud_layout.progress_bar_height;
        app_.ts->DrawRect(barStart, barY, barEnd, barY + barHeight, fgColor);

        if (snapshot.draw_page_count > 1 && current_book &&
            current_book->GetPosition() > 0)
        {
          int fillW = (int)(((float)(barEnd - barStart - 4) *
                             current_book->GetPosition()) /
                            (snapshot.draw_page_count - 1));
          if (fillW > 0)
          {
            app_.ts->FillRect(barStart + 2, barY + 2, barStart + 2 + fillW,
                              barY + barHeight - 2, fgColor);
          }
        }
      }
    }
    app_.ts->margin.bottom = savedBottomMargin;
  }
  app_.ts->SetStyle(style);
  app_.ts->SetScreen(screen);
  last_minute_ = minute_of_day;
  last_display_token_ =
      (mode == AppMode::Book && UsesFixedLayoutMinimalHud(current_book))
          ? (current_book ? (int)current_book->GetPosition() : -1)
          : snapshot.percent_tenths;
  force_redraw_ = false;
}
