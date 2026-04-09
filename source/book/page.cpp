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

#include "app/app.h"
#include "book/book.h"
#include "book/page_buffer_utils.h"
#include "debug_log.h"
#include "shared/text_render_layout_utils.h"
#include <algorithm>
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
  const bool saved_auto_wrap = ts->IsAutoWrapEnabled();
  const bool saved_clip_to_content = ts->IsClipToContentEnabled();
  // Reflowed page buffers already carry explicit line breaks/wrap decisions.
  // Runtime per-glyph wrapping in TextRenderer breaks RTL line anchoring.
  ts->SetAutoWrapEnabled(false);
  ts->SetClipToContentEnabled(true);

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
  bool underline = false;
  bool strikethrough = false;
  bool superscript = false;
  bool subscript = false;
  bool mono = false;

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
  const int first_bottom_margin = first_is_left ? leftBottomMargin : rightBottomMargin;
  const int second_bottom_margin = first_is_left ? rightBottomMargin : leftBottomMargin;
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
  ts->margin.bottom =
      text_render_layout_utils::ResolveReadingScreenMetrics(
          true, first_is_left, leftBottomMargin, rightBottomMargin)
          .bottom_margin;
  // Clear both page buffers through Text API so dirty flags stay coherent.
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  ts->SetScreen(first_screen);

  u16 i = 0;
  InlineImageContext next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
  bool rtl_paragraph = false;
  u32 rtl_line_px = 0;  // parse-time line width stashed by TEXT_RTL_LINE_PX
  bool stopped_on_render_break = false;
  const char *render_break_reason = "complete";
  int render_break_index = -1;
  int newline_count_first = 0;
  int newline_count_second = 0;
  bool first_screen_had_content = false;
  bool second_screen_had_content = false;
  while (i < length) {
    u32 c = buf[i];
    if (c == TEXT_PARAGRAPH_RTL) {
      rtl_paragraph = true;
      i++;
      continue;
    } else if (c == TEXT_PARAGRAPH_LTR) {
      rtl_paragraph = false;
      rtl_line_px = 0;
      i++;
      continue;
    } else if (c == TEXT_RTL_LINE_PX) {
      if (i + 1 < length)
        rtl_line_px = buf[i + 1];
      // TEXT_RTL_LINE_PX is only ever emitted for RTL content, so its presence
      // implies RTL paragraph mode even when TEXT_PARAGRAPH_RTL is absent
      // (e.g. a paragraph that spans page boundaries: the continuation page
      // has RTL_LINE_PX tokens but no leading PARAGRAPH_RTL token).
      rtl_paragraph = true;
      // This token always begins a new RTL line. Reset linebegan so the
      // RTL_ALIGN check fires for the first glyph of this line, even when
      // ts->linebegan was left true by the previous page's draw loop.
      ts->linebegan = false;
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF_CAT(ts->app, DBG_LEVEL_DEBUG, DBG_CAT_LAYOUT,
                   "RTL token px=%u i=%u linebegan=%d rtl=%d",
                   (unsigned)rtl_line_px, (unsigned)i,
                   ts->linebegan ? 1 : 0, rtl_paragraph ? 1 : 0);
#endif
      i += 2;
      continue;
    } else if (c == '\n') {
      // line break, page breaking if necessary
      i++;
      next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;

      const text_render_layout_utils::ReadingScreenMetrics metrics =
          text_render_layout_utils::ResolveReadingScreenMetrics(
              on_first_screen, first_is_left, leftBottomMargin,
              rightBottomMargin);
      int maxHeight = metrics.max_height;
      int currentBottomMargin = metrics.bottom_margin;
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
        {
          stopped_on_render_break = true;
          render_break_reason = "newline-overflow-second-screen";
          render_break_index = (int)i;
          break;
        }
      } else if (ts->linebegan) {
        ts->PrintNewLine();
        if (on_first_screen)
          newline_count_first++;
        else
          newline_count_second++;
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
    } else if (c == TEXT_UNDERLINE_ON) {
      i++;
      underline = true;
    } else if (c == TEXT_UNDERLINE_OFF) {
      i++;
      underline = false;
    } else if (c == TEXT_STRIKETHROUGH_ON) {
      i++;
      strikethrough = true;
    } else if (c == TEXT_STRIKETHROUGH_OFF) {
      i++;
      strikethrough = false;
    } else if (c == TEXT_SUPERSCRIPT_ON) {
      i++;
      superscript = true;
      subscript = false;
    } else if (c == TEXT_SUPERSCRIPT_OFF) {
      i++;
      superscript = false;
    } else if (c == TEXT_SUBSCRIPT_ON) {
      i++;
      subscript = true;
      superscript = false;
    } else if (c == TEXT_SUBSCRIPT_OFF) {
      i++;
      subscript = false;
    } else if (c == TEXT_MONO_ON) {
      i++;
      mono = true;
    } else if (c == TEXT_MONO_OFF) {
      i++;
      mono = false;
    } else if (c == TEXT_HR) {
      i++;
      const int x0 = ts->margin.left;
      const int x1 = ts->display.width - ts->margin.right;
      const int y = ts->GetPenY() + ts->GetHeight() / 2;
      ts->FillRect(x0, y, x1, y + 1, ts->GetFgColor());
      if (!ts->PrintNewLine()) {
        stopped_on_render_break = true;
        render_break_reason = "hr-newline-failed";
        render_break_index = (int)i;
        break;
      }
      if (on_first_screen)
        newline_count_first++;
      else
        newline_count_second++;
      ts->linebegan = false;
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
          if (!advance_to_next_screen()) {
            stopped_on_render_break = true;
            render_break_reason = "image-advance-before-no-next-screen";
            render_break_index = (int)i;
            break;
          }
        }
        if (image_plan.line_break_before && ts->linebegan) {
          if (!ts->PrintNewLine()) {
            stopped_on_render_break = true;
            render_break_reason = "image-line-break-before-failed";
            render_break_index = (int)i;
            break;
          }
          if (on_first_screen)
            newline_count_first++;
          else
            newline_count_second++;
          ts->linebegan = false;
        }

        book->DrawInlineImage(ts, image_id, &image_plan);
        if (on_first_screen)
          first_screen_had_content = true;
        else
          second_screen_had_content = true;

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
          if (!advance_to_next_screen()) {
            stop_page_draw = true;
            stopped_on_render_break = true;
            render_break_reason = "page-image-no-next-screen";
            render_break_index = (int)i;
          }
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

      if (rtl_paragraph && !ts->linebegan) {
        int line_width;
        if (rtl_line_px > 0) {
          // Use the parse-time pixel width stored by TEXT_RTL_LINE_PX. This
          // avoids render-time font measurement for Arabic presentation forms,
          // which can return notdef advances when fallback fonts are involved.
          line_width = (int)rtl_line_px;
          rtl_line_px = 0;
        } else {
          // Fallback: re-measure by scanning forward (used for LTR text in
          // mixed paragraphs or legacy page buffers without the token).
          line_width = 0;
          for (u16 scan = (u16)(i - 1); scan < length; scan++) {
            u32 sc = buf[scan];
            if (sc == '\n' || sc < 32)
              break;
            line_width += ts->GetAdvance((u16)sc);
          }
        }
        int right_edge = ts->display.width - ts->margin.right;
        int rtl_x = text_render_layout_utils::ComputeRtlLineStartX(
            ts->margin.left, right_edge, line_width);
#ifdef DSLIBRIS_DEBUG
        DBG_LOGF_CAT(
            ts->app, DBG_LEVEL_DEBUG, DBG_CAT_LAYOUT,
            "RTL line anchor width=%d right=%d start_x=%d clip=[%d,%d) y=%d side=%s",
            line_width, right_edge, rtl_x, (int)ts->margin.left, right_edge,
            (int)ts->GetPenY(), on_first_screen ? "first" : "second");
#endif
        ts->SetPen((u16)rtl_x, ts->GetPenY());
      }

      const int glyph_x0 = (int)ts->GetPenX();
      const int base_pen_y = (int)ts->GetPenY();
      if (superscript || subscript) {
        const int y_offset =
            superscript ? -std::max(2, ts->GetHeight() / 3)
                        : std::max(2, ts->GetHeight() / 4);
        const int shifted_y = std::max(0, base_pen_y + y_offset);
        ts->SetPen((u16)glyph_x0, (u16)shifted_y);
      }
      if (mono && ts->bold && ts->italic)
        ts->PrintChar(c, TEXT_STYLE_MONO_BOLDITALIC);
      else if (mono && ts->bold)
        ts->PrintChar(c, TEXT_STYLE_MONO_BOLD);
      else if (mono && ts->italic)
        ts->PrintChar(c, TEXT_STYLE_MONO_ITALIC);
      else if (mono)
        ts->PrintChar(c, TEXT_STYLE_MONO);
      else if (ts->bold && ts->italic)
        ts->PrintChar(c, TEXT_STYLE_BOLDITALIC);
      else if (ts->italic)
        ts->PrintChar(c, TEXT_STYLE_ITALIC);
      else if (ts->bold)
        ts->PrintChar(c, TEXT_STYLE_BOLD);
      else
        ts->PrintChar(c, TEXT_STYLE_REGULAR);

      const int glyph_x1 = (int)ts->GetPenX();
      if (superscript || subscript)
        ts->SetPen((u16)glyph_x1, (u16)base_pen_y);
      const int baseline_y = (int)ts->GetPenY();
      const u16 deco_color = ts->GetFgColor();
      if (glyph_x1 > glyph_x0) {
        if (underline) {
          const int y = baseline_y + 1;
          if (y >= 0)
            ts->FillRect((u16)glyph_x0, (u16)y, (u16)glyph_x1, (u16)(y + 1),
                         deco_color);
        }
        if (strikethrough) {
          const int y = baseline_y - std::max(2, ts->GetHeight() / 3);
          if (y >= 0)
            ts->FillRect((u16)glyph_x0, (u16)y, (u16)glyph_x1, (u16)(y + 1),
                         deco_color);
        }
      }

      ts->linebegan = true;
      if (on_first_screen)
        first_screen_had_content = true;
      else
        second_screen_had_content = true;
    }
  }

#ifdef DSLIBRIS_DEBUG
  static int s_page_draw_diag_budget = 48;
  if (ts->app && s_page_draw_diag_budget > 0) {
    DBG_LOGF_CAT(
        ts->app, DBG_LEVEL_INFO, DBG_CAT_LAYOUT,
        "PAGE draw pos=%u len=%u consumed=%u stop=%d reason=%s idx=%d first_content=%d second_content=%d nl_first=%d nl_second=%d final_screen=%s pen=%u,%u",
        (unsigned)(book ? book->GetPosition() : 0), (unsigned)length,
        (unsigned)i, stopped_on_render_break ? 1 : 0, render_break_reason,
        render_break_index, first_screen_had_content ? 1 : 0,
        second_screen_had_content ? 1 : 0, newline_count_first,
        newline_count_second,
        (ts->GetScreen() == second_screen) ? "second" : "first",
        (unsigned)ts->GetPenX(), (unsigned)ts->GetPenY());
    s_page_draw_diag_budget--;
  }
#endif

  DrawNumber(ts, second_screen);
#ifdef OFFSCREEN
  ts->SetScreen(second_screen);
  ts->CopyScreen(ts->offscreen, ts->screen);
  ts->SetScreen(pushscreen);
#endif
  ts->SetAutoWrapEnabled(saved_auto_wrap);
  ts->SetClipToContentEnabled(saved_clip_to_content);
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
