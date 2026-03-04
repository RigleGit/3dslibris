/*
 Copyright (C) 2007-2009 Ray Haleblian

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

#include "page.h"

#include <list>
#include <string.h>
#include <time.h>

Page::Page(Book *b) {
  book = b;
  buf = NULL;
  length = 0;
  start = 0;
}

Page::~Page() {
  if (buf)
    delete buf;
}

u8 Page::SetBuffer(u8 *src, u16 len) {
  if (buf)
    delete[] buf;
  buf = new u8[len];
  memcpy(buf, src, len);
  length = len;
  return 0;
}

void Page::Cache(FILE *fp) {
  if (!buf)
    return;
  fwrite((const char *)buf, 1, strlen((char *)buf), fp);
}

#if 0
void Page::Draw()
{
	Text *ts = book->app->ts;
	if(ts) Draw(ts);
}
#endif

void Page::Draw(Text *ts) {
  char msg[128];
  sprintf(msg, "Drawing page: %d bytes", length);
  book->GetApp()->PrintStatus(msg);

  //! Write to offscreen buffer, then blit to video memory, for both screens.
  ts->InitPen();
  ts->linebegan = false;
  ts->italic = false;
  ts->bold = false;

#ifdef OFFSCREEN
  // Draw offscreen.
  auto pushscreen = ts->screen;
  ts->SetScreen(ts->offscreen);
#else
  ts->SetScreen(ts->screenleft);
#endif
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();

  u16 i = 0;
  while (i < length) {
    u32 c = buf[i];
    if (c == '\n') {
      // line break, page breaking if necessary
      i++;

      int maxHeight = (ts->GetScreen() == ts->screenleft) ? 400 : 320;
      if (ts->GetPenY() + ts->GetHeight() + ts->linespacing >
          maxHeight - ts->margin.bottom) {
        // Move to right page
        if (ts->GetScreen() == ts->screenleft) {
#ifdef OFFSCREEN
          ts->SetScreen(ts->screenleft);
          ts->CopyScreen(ts->offscreen, ts->screen);
          ts->SetScreen(ts->offscreen);
#else
          ts->SetScreen(ts->screenright);
#endif
          ts->ClearScreen();
          ts->InitPen();
          ts->linebegan = false;
        } else
          break;
      } else if (ts->linebegan) {
        ts->PrintNewLine();
      }
    } else if (c == TEXT_BOLD_ON) {
      i++;
      ts->bold = true;
    } else if (c == TEXT_BOLD_OFF) {
      i++;
      ts->bold = false;
    } else if (c == TEXT_ITALIC_ON) {
      i++;
      ts->italic = true;
    } else if (c == TEXT_ITALIC_OFF) {
      i++;
      ts->italic = false;
    } else {
      if (c > 127)
        i += ts->GetCharCode((char *)&(buf[i]), &c);
      else
        i++;

      if (ts->bold && ts->italic)
        ts->PrintChar(c, TEXT_STYLE_BOLDITALIC);
      else if (ts->italic)
        ts->PrintChar(c, TEXT_STYLE_ITALIC);
      else if (ts->bold)
        ts->PrintChar(c, TEXT_STYLE_BOLD);
      else
        ts->PrintChar(c, TEXT_STYLE_REGULAR);

      ts->linebegan = true;
    }
  }

  DrawNumber(ts);
#ifdef OFFSCREEN
  ts->SetScreen(ts->screenright);
  ts->CopyScreen(ts->offscreen, ts->screen);
  ts->SetScreen(pushscreen);
#endif
}

void Page::DrawNumber(Text *ts) {
  //! Draw page number on current screen.
  char msg[128];

  // Find out if the page is bookmarked or not
  bool isBookmark = false;
  u16 pagecurrent = book->GetPosition();
  u16 pagecount = book->GetPageCount();
  std::list<u16> *bookmarks = book->GetBookmarks();
  for (std::list<u16>::iterator i = bookmarks->begin(); i != bookmarks->end();
       i++) {
    if (*i == pagecurrent) {
      isBookmark = true;
      break;
    }
  }
  if (isBookmark) {
    if (pagecount == 1)
      sprintf((char *)msg, "[ %d* ]", pagecurrent + 1);
    else if (pagecurrent == 0)
      sprintf((char *)msg, "[ %d* >", pagecurrent + 1);
    else if (pagecurrent == pagecount - 1)
      sprintf((char *)msg, "< %d* ]", pagecurrent + 1);
    else
      sprintf((char *)msg, "< %d* >", pagecurrent + 1);
  } else {
    if (pagecount == 1)
      sprintf((char *)msg, "[ %d ]", pagecurrent + 1);
    else if (pagecurrent == 0)
      sprintf((char *)msg, "[ %d >", pagecurrent + 1);
    else if (pagecurrent == pagecount - 1)
      sprintf((char *)msg, "< %d ]", pagecurrent + 1);
    else
      sprintf((char *)msg, "< %d >", pagecurrent + 1);
  }

  // Position page number in horizontal proportion
  // to our current progress in the book.
  int stringwidth = ts->GetStringAdvance(msg);
  // Put it at the bottom-right corner of the right screen.
  int location = ts->display.width - ts->margin.right - stringwidth - 4;

  ts->SetScreen(
      ts->screenright); // Screen 1 is the physically 320px tall screen
  ts->SetPen((u8)location, 310);
  ts->PrintString(msg);

  // Add clock, progress bar, and percentage on the left screen
  if (pagecount > 0) {
    char tmsg[32];
    char pctmsg[16];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int pct = (pagecount > 1) ? (pagecurrent * 100) / (pagecount - 1) : 100;

    // Format time
    if (tm) {
      bool is_24h = book->GetApp()->prefs->time24h;
      if (is_24h) {
        sprintf(tmsg, "%02d:%02d", tm->tm_hour, tm->tm_min);
      } else {
        int h = tm->tm_hour % 12;
        if (h == 0)
          h = 12;
        const char *ampm = (tm->tm_hour >= 12) ? "PM" : "AM";
        sprintf(tmsg, "%d:%02d%s", h, tm->tm_min, ampm);
      }
    } else {
      sprintf(tmsg, "--:--");
    }
    sprintf(pctmsg, "%d%%", pct);

    ts->SetScreen(ts->screenleft);

    // Draw progress bar at y=370, spanning the usable width
    int barLeft = ts->margin.left;
    int barRight = ts->display.width - ts->margin.right;
    int barY = 368;
    int barH = 4;
    int w = ts->display.height; // buffer stride
    float progress =
        (pagecount > 1) ? (float)pagecurrent / (float)(pagecount - 1) : 1.0f;
    int fillWidth = (int)((barRight - barLeft) * progress);

    // Bar background (light gray)
    u16 bgColor = 0xBDD7;
    for (int y = barY; y < barY + barH && y < 400; y++) {
      for (int x = barLeft; x < barRight && x < 240; x++) {
        ts->screenleft[y * w + x] = bgColor;
      }
    }

    // Bar fill (dark gray/black)
    u16 fillColor = 0x4A49; // dark gray
    for (int y = barY; y < barY + barH && y < 400; y++) {
      for (int x = barLeft; x < barLeft + fillWidth && x < 240; x++) {
        ts->screenleft[y * w + x] = fillColor;
      }
    }

    // Draw bookmark indicators on the progress bar
    for (std::list<u16>::iterator bi = bookmarks->begin();
         bi != bookmarks->end(); bi++) {
      int bx = barLeft + (int)((float)(*bi) / (float)(pagecount - 1) *
                               (barRight - barLeft));
      if (bx >= barLeft && bx < barRight) {
        // Draw small mark (2px wide, full bar height + 1 above)
        u16 markColor = 0x0000; // black
        for (int y = barY - 1; y < barY + barH + 1 && y < 400; y++) {
          for (int dx = 0; dx < 2 && (bx + dx) < barRight; dx++) {
            if (y >= 0)
              ts->screenleft[y * w + (bx + dx)] = markColor;
          }
        }
      }
    }

    // Draw time on the left, percentage on the right below the bar
    ts->SetPen(barLeft, 385);
    ts->PrintString(tmsg);
    int pwidth = ts->GetStringAdvance(pctmsg);
    ts->SetPen(barRight - pwidth, 385);
    ts->PrintString(pctmsg);
  }
}
