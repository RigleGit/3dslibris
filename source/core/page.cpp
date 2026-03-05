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
    delete[] buf;
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
  fwrite((const char *)buf, 1, length, fp);
}

#if 0
void Page::Draw()
{
	Text *ts = book->app->ts;
	if(ts) Draw(ts);
}
#endif

void Page::Draw(Text *ts) {
  int savedBottomMargin = ts->margin.bottom;
  int leftBottomMargin = savedBottomMargin;
  // On the 320px screen we only need a small footer for page number.
  int rightBottomMargin =
      (savedBottomMargin > 16) ? 16 : savedBottomMargin;

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
  ts->margin.bottom = leftBottomMargin;
  // Force-clear both page buffers explicitly to avoid stale artifacts.
  const u16 bg = ts->GetBgColor();
  const int bufsize = PAGE_HEIGHT * PAGE_HEIGHT;
  for (int k = 0; k < bufsize; k++) {
    ts->screenright[k] = bg;
    ts->screenleft[k] = bg;
  }
  ts->SetScreen(ts->screenleft);

  u16 i = 0;
  while (i < length) {
    u32 c = buf[i];
    if (c == '\n') {
      // line break, page breaking if necessary
      i++;

      int maxHeight = (ts->GetScreen() == ts->screenleft) ? 400 : 320;
      int currentBottomMargin =
          (ts->GetScreen() == ts->screenleft) ? leftBottomMargin
                                              : rightBottomMargin;
      ts->margin.bottom = currentBottomMargin;
      if (ts->GetPenY() + ts->GetHeight() + ts->linespacing >
          maxHeight - currentBottomMargin) {
        // Move to right page
        if (ts->GetScreen() == ts->screenleft) {
#ifdef OFFSCREEN
          ts->SetScreen(ts->screenleft);
          ts->CopyScreen(ts->offscreen, ts->screen);
          ts->SetScreen(ts->offscreen);
#else
          ts->SetScreen(ts->screenright);
#endif
          ts->margin.bottom = rightBottomMargin;
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
    } else if (c == TEXT_IMAGE) {
      if (i + 2 < length) {
        u16 image_id = ((u16)buf[i + 1] << 8) | (u16)buf[i + 2];
        i += 3;
        book->DrawInlineImage(ts, image_id);
        ts->linebegan = false;
      } else {
        i++;
      }
    } else {
      if (c > 127)
        i += ts->GetCharCode((char *)&(buf[i]), &c);
      else
        i++;

      ts->margin.bottom = (ts->GetScreen() == ts->screenleft)
                              ? leftBottomMargin
                              : rightBottomMargin;

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
  ts->margin.bottom = savedBottomMargin;
}

void Page::DrawNumber(Text *ts) {
  //! Draw page number on current screen.
  char msg[64];

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

  // UI elements should not be clipped by page margins.
  int savedBottomMargin = ts->margin.bottom;
  ts->margin.bottom = 0;

  ts->SetScreen(ts->screenright); // Screen 1 is the physically 320px tall screen
  ts->SetPen((u8)location, 310);
  ts->PrintString(msg);
  ts->margin.bottom = savedBottomMargin;
}
