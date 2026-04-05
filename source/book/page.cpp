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

/*
  3DS port modifications by Rigle (summary):
  - Updated page draw flow for 3DS text buffers and status overlays.
  - Integrated inline-image draw tokens and page number placement.
  - Added safer clipping/margin behavior for rotated layouts.
*/

#include "book/page.h"

#include "book/book.h"
#include "book/page_buffer_utils.h"
#include <list>
#include <string.h>
#include <time.h>

Page::Page(Book *b) {
  book = b;
  buf = NULL;
  length = 0;
  capacity = 0;
  start = 0;
  end = 0;
}

Page::~Page() { buf = NULL; }

void Page::SyncBufferAlias() {
  buf = storage.empty() ? NULL : storage.data();
  length = (int)storage.size();
  capacity = (int)storage.capacity();
}

void Page::SetBuffer(const u32 *src, int len) {
  if (len <= 0) {
    std::vector<u32>().swap(storage);
    SyncBufferAlias();
    return;
  }

  const size_t required_capacity =
      page_buffer_utils::RequiredPageBufferCodepoints((size_t)capacity,
                                                      (size_t)len);
  if ((size_t)capacity < required_capacity)
    storage.reserve(required_capacity);

  storage.resize(len);
  if (src)
    memcpy(storage.data(), src, len * sizeof(u32));
  SyncBufferAlias();
}

void Page::AdoptBuffer(page_buffer_utils::OwnedPageBuffer *owned) {
  if (!owned) {
    SetBuffer(NULL, 0);
    return;
  }

  const size_t len = owned->codepoints.size();
  if (len == 0) {
    SetBuffer(NULL, 0);
    owned->codepoints.clear();
    return;
  }

  storage.swap(owned->codepoints);
  SyncBufferAlias();
}

void Page::Draw(Text *ts) {
  int savedBottomMargin = ts->margin.bottom;
  int leftBottomMargin = savedBottomMargin;
  // On the 320px screen we only need a small footer for page number.
  int rightBottomMargin =
      (savedBottomMargin > 16) ? 16 : savedBottomMargin;
  const bool turned_right = (book && book->GetOrientation() != 0);
  u16 *first_screen = turned_right ? ts->screenright : ts->screenleft;
  u16 *second_screen = turned_right ? ts->screenleft : ts->screenright;

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
  ts->SetScreen(first_screen);
#endif

  // Cache screen-dependent values to avoid repeated comparisons in the hot loop.
  // first_screen == screenleft → leftBottomMargin, maxHeight=400
  // first_screen == screenright → rightBottomMargin, maxHeight=320
  const bool first_is_left = (first_screen == ts->screenleft);
  int first_max_height = first_is_left ? 400 : 320;
  int first_bottom_margin = first_is_left ? leftBottomMargin : rightBottomMargin;
  int second_bottom_margin = first_is_left ? rightBottomMargin : leftBottomMargin;
  bool on_first_screen = true;

  auto advance_to_next_screen = [&]() -> bool {
    if (ts->GetScreen() == first_screen) {
#ifdef OFFSCREEN
      ts->SetScreen(second_screen);
      ts->CopyScreen(ts->offscreen, ts->screen);
      ts->SetScreen(ts->offscreen);
#else
      ts->SetScreen(second_screen);
#endif
      on_first_screen = false;
      ts->margin.bottom = second_bottom_margin;
      ts->ClearScreen();
      ts->InitPen();
      ts->linebegan = false;
      return true;
    }
    return false;
  };
  ts->margin.bottom = first_bottom_margin;
  // Clear both page buffers through Text API so dirty flags stay coherent.
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  ts->SetScreen(first_screen);

  u16 i = 0;
  InlineImageContext next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
  while (i < length) {
    u32 c = buf[i];
    if (c == '\n') {
      // line break, page breaking if necessary
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;

      int maxHeight = first_max_height;
      int currentBottomMargin = first_bottom_margin;
      ts->margin.bottom = currentBottomMargin;
      if (ts->GetPenY() + ts->GetHeight() + ts->linespacing >
          maxHeight - currentBottomMargin) {
        // Move to second page
        if (ts->GetScreen() == first_screen) {
#ifdef OFFSCREEN
          ts->SetScreen(second_screen);
          ts->CopyScreen(ts->offscreen, ts->screen);
          ts->SetScreen(ts->offscreen);
#else
          ts->SetScreen(second_screen);
#endif
          on_first_screen = false;
          ts->margin.bottom = second_bottom_margin;
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
    } else if (c == TEXT_IMAGE_CONTEXT_DEFAULT) {
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
    } else if (c == TEXT_IMAGE_LEADING_PARAGRAPH) {
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    } else if (c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
    } else if (c == TEXT_IMAGE) {
      if (i + 1 < length) {
        u16 image_id = (u16)buf[i + 1];
        i += 2;

        InlineImageLayoutPlan image_plan{};
        int current_screen = (ts->GetScreen() == first_screen) ? 0 : 1;
        book->PlanInlineImageLayout(ts, image_id, current_screen, ts->GetPenX(),
                                    ts->GetPenY(), ts->linebegan, next_image_context,
                                    &image_plan);
        next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;

        if (image_plan.advance_before) {
          if (!advance_to_next_screen())
            break;
        }
        if (image_plan.line_break_before && ts->linebegan) {
          if (!ts->PrintNewLine())
            break;
          ts->linebegan = false;
        }

        book->DrawInlineImage(ts, image_id, &image_plan);

        bool stop_page_draw = false;
        switch (image_plan.mode) {
        case INLINE_IMAGE_LAYOUT_INLINE:
          ts->SetPen(ts->GetPenX() + image_plan.draw_width + ts->GetAdvance(' '),
                     ts->GetPenY());
          ts->linebegan = true;
          break;

        case INLINE_IMAGE_LAYOUT_BAND:
          // Band images occupy a vertical block; normal text continues below.
          ts->SetPen(ts->margin.left,
                     ts->GetPenY() + image_plan.vertical_space_after_draw);
          ts->linebegan = false;
          break;

        case INLINE_IMAGE_LAYOUT_PAGE:
        default:
          if (!advance_to_next_screen())
            stop_page_draw = true;
          ts->linebegan = false;
          break;
        }
        if (stop_page_draw)
          break;
      } else {
        i++;
        next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
      }
    } else {
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;

      ts->margin.bottom = (on_first_screen == first_is_left)
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

  DrawNumber(ts, second_screen);
#ifdef OFFSCREEN
  ts->SetScreen(second_screen);
  ts->CopyScreen(ts->offscreen, ts->screen);
  ts->SetScreen(pushscreen);
#endif
  ts->margin.bottom = savedBottomMargin;
}

void Page::DrawNumber(Text *ts, u16 *number_screen) {
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
      snprintf((char *)msg, sizeof(msg), "[ %d* ]", pagecurrent + 1);
    else if (pagecurrent == 0)
      snprintf((char *)msg, sizeof(msg), "[ %d* >", pagecurrent + 1);
    else if (pagecurrent == pagecount - 1)
      snprintf((char *)msg, sizeof(msg), "< %d* ]", pagecurrent + 1);
    else
      snprintf((char *)msg, sizeof(msg), "< %d* >", pagecurrent + 1);
  } else {
    if (pagecount == 1)
      snprintf((char *)msg, sizeof(msg), "[ %d ]", pagecurrent + 1);
    else if (pagecurrent == 0)
      snprintf((char *)msg, sizeof(msg), "[ %d >", pagecurrent + 1);
    else if (pagecurrent == pagecount - 1)
      snprintf((char *)msg, sizeof(msg), "< %d ]", pagecurrent + 1);
    else
      snprintf((char *)msg, sizeof(msg), "< %d >", pagecurrent + 1);
  }

  // Position page number in horizontal proportion
  // to our current progress in the book.
  int stringwidth = ts->GetStringAdvance(msg);
  // Put it at the bottom-right corner of the second reading screen.
  int location = ts->display.width - ts->margin.right - stringwidth - 4;

  // UI elements should not be clipped by page margins.
  int savedBottomMargin = ts->margin.bottom;
  ts->margin.bottom = 0;

  u16 *target = number_screen ? number_screen : ts->screenright;
  ts->SetScreen(target);
  const int baseline_y = (target == ts->screenleft) ? 390 : 310;
  ts->SetPen((u8)location, (u16)baseline_y);
  ts->PrintString(msg);
  ts->margin.bottom = savedBottomMargin;
}
