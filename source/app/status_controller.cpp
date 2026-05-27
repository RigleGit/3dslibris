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
#include "book/page.h"
#include "shared/app_flow_utils.h"
#include "shared/battery_utils.h"
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

  // Formats the current time into buf. Supports 12h (with AM/PM) and 24h modes.
  // Falls back to "--:--" if timeStruct is null.
  static void FormatClockString(char *buf, size_t bufsz, const struct tm *t,
                                bool time24h)
  {
    if (!t)
    {
      snprintf(buf, bufsz, "--:--");
    }
    else if (time24h)
    {
      snprintf(buf, bufsz, "%02d:%02d", t->tm_hour, t->tm_min);
    }
    else
    {
      int h = t->tm_hour % 12;
      if (h == 0)
        h = 12;
      snprintf(buf, bufsz, "%02d:%02d %s", h, t->tm_min,
               t->tm_hour >= 12 ? "PM" : "AM");
    }
  }

  static void FormatEtaMinutes(char *buf, size_t bufsz, const char *prefix,
                               int total_minutes)
  {
    if (!buf || bufsz == 0 || !prefix) {
      return;
    }
    if (total_minutes < 0) {
      snprintf(buf, bufsz, "%s --", prefix);
      return;
    }
    const int hours = total_minutes / 60;
    const int mins = total_minutes % 60;
    if (hours > 0)
      snprintf(buf, bufsz, "%s %dh%02dm", prefix, hours, mins);
    else
      snprintf(buf, bufsz, "%s %dm", prefix, mins);
  }

  static bool FormatEtaPair(char *chapter_buf, size_t chapter_sz,
                            char *book_buf, size_t book_sz,
                            const Book *book)
  {
    if (!chapter_buf || chapter_sz == 0 || !book_buf || book_sz == 0 ||
        !book || !book->HasReadingPaceEstimate())
      return false;

    const int book_min = book->EstimateRemainingBookMinutes();
    if (book_min < 0)
      return false;

    const int chapter_min = book->EstimateRemainingChapterMinutes();
    FormatEtaMinutes(chapter_buf, chapter_sz, "ch", chapter_min);
    FormatEtaMinutes(book_buf, book_sz, "bk", book_min);
    return true;
  }
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
    progress_lock_book_ = const_cast<Book *>(snapshot.next_locked_book);
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

  const bool use_eta_token =
      mode == AppMode::Book && app_.prefs->show_time_remaining &&
      !UsesFixedLayoutMinimalHud(current_book);
  const int display_token =
      (mode == AppMode::Book && UsesFixedLayoutMinimalHud(current_book))
          ? (current_book ? (int)current_book->GetPosition() : -1)
          : (use_eta_token ? (current_book ? (int)current_book->GetPosition() : -1)
                           : snapshot.percent_tenths);

  if (!force_redraw_ && minute_of_day == last_minute_ &&
      display_token == last_display_token_)
  {
    return;
  }

  char tmsg[24];
  FormatClockString(tmsg, sizeof(tmsg), timeStruct, app_.prefs->time24h);

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

    u8 batt_percent = 0;
    bool batt_charging = false;
    bool batt_approx = false;
    char bmsg[12] = {};
    int battWidth = 0;
    if (battery_utils::ReadBatteryState(&batt_percent, &batt_charging,
                                        &batt_approx))
    {
      battery_utils::FormatBatteryString(bmsg, sizeof(bmsg), batt_percent,
                                         batt_charging, batt_approx);
      battWidth = app_.ts->GetStringWidth(bmsg, TEXT_STYLE_BROWSER);
      app_.ts->SetPen(8 + clockWidth + 8, textY);
      app_.ts->PrintString(bmsg);
    }

    // Inline-link hint: shown at the far right of the status bar when the
    // current page has links. Shifts the progress % left to make room.
    int right_edge = 232;
    if (mode == AppMode::Book)
    {
      const char *hint = app_.IsInlineLinkFocusActive() ? "A:go" : "Y:lnk";
      if (hint)
      {
        int hw = app_.ts->GetStringWidth(hint, TEXT_STYLE_BROWSER);
        int hx = 232 - hw;
        app_.ts->SetPen(hx, textY);
        app_.ts->PrintString(hint);
        right_edge = hx - 4;
      }
    }

    int pX = right_edge;
    if (mode == AppMode::Opening)
    {
      int pw = app_.ts->GetStringWidth(kOpeningStatusLabel, TEXT_STYLE_BROWSER);
      pX = right_edge - pw;
      app_.ts->SetPen(pX, textY);
      app_.ts->PrintString(kOpeningStatusLabel);
    }
    else if (snapshot.has_progress)
    {
      char pmsg[32];
      snprintf(pmsg, sizeof(pmsg), "%.1f%%", snapshot.percent_value);
      int pw = app_.ts->GetStringWidth(pmsg, TEXT_STYLE_BROWSER);
      pX = right_edge - pw;
      app_.ts->SetPen(pX, textY);
      app_.ts->PrintString(pmsg);

      int barStart = 8 + clockWidth + (battWidth > 0 ? 8 + battWidth + 8 : 12);
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

        if (app_.prefs->show_time_remaining && current_book) {
          char eta_ch[24];
          char eta_bk[24];
          if (FormatEtaPair(eta_ch, sizeof(eta_ch), eta_bk, sizeof(eta_bk),
                            current_book)) {
            const int etaY = hud_layout.clear_bottom - app_.ts->GetHeight() - 1;
            const int eta_bk_w =
                app_.ts->GetStringWidth(eta_bk, TEXT_STYLE_BROWSER);
            app_.ts->SetPen(8, etaY);
            app_.ts->PrintString(eta_ch);
            app_.ts->SetPen(std::max(8, 232 - eta_bk_w), etaY);
            app_.ts->PrintString(eta_bk);
          }
        }
      }
    }
    app_.ts->margin.bottom = savedBottomMargin;
  }
  app_.ts->SetStyle(style);
  app_.ts->SetScreen(screen);
  last_minute_ = minute_of_day;
  last_display_token_ = display_token;
  force_redraw_ = false;
}
