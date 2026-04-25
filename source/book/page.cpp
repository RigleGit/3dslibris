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
#include "book/page_alignment_utils.h"
#include "book/book_xml_css_style_utils.h"
#include "book/page_buffer_utils.h"
#include "debug_log.h"
#include "shared/text_render_layout_utils.h"
#include <algorithm>
#include <list>
#include <string.h>
#include <time.h>

namespace {

void DrawSolidDecoration(Text *ts, int x0, int x1, int y, u16 color) {
  if (!ts || x1 <= x0 || y < 0)
    return;
  ts->FillRect((u16)x0, (u16)y, (u16)x1, (u16)(y + 1), color);
}

void DrawPatternedUnderline(Text *ts, int x0, int x1, int y, u16 color,
                            u8 underline_style) {
  if (!ts || x1 <= x0 || y < 0)
    return;
  switch (underline_style) {
  case UNDERLINE_STYLE_DOTTED:
    for (int x = x0; x < x1; x += 2)
      ts->FillRect((u16)x, (u16)y, (u16)std::min(x + 1, x1), (u16)(y + 1),
                   color);
    break;
  case UNDERLINE_STYLE_DASHED:
    for (int x = x0; x < x1; x += 5)
      ts->FillRect((u16)x, (u16)y, (u16)std::min(x + 3, x1), (u16)(y + 1),
                   color);
    break;
  case UNDERLINE_STYLE_WAVY:
    for (int x = x0; x < x1; x++) {
      const int y_offset = ((x - x0) % 4 < 2) ? 0 : 1;
      ts->FillRect((u16)x, (u16)(y + y_offset), (u16)(x + 1),
                   (u16)(y + y_offset + 1), color);
    }
    break;
  case UNDERLINE_STYLE_SOLID:
  default:
    DrawSolidDecoration(ts, x0, x1, y, color);
    break;
  }
}

void ExpandLinkBounds(inline_link_utils::LinkRect *rect, int x0, int y0, int x1,
                      int y1) {
  if (!rect || x1 <= x0 || y1 <= y0)
    return;
  if (!inline_link_utils::IsValidRect(*rect)) {
    rect->x0 = x0;
    rect->y0 = y0;
    rect->x1 = x1;
    rect->y1 = y1;
    return;
  }
  rect->x0 = std::min(rect->x0, x0);
  rect->y0 = std::min(rect->y0, y0);
  rect->x1 = std::max(rect->x1, x1);
  rect->y1 = std::max(rect->y1, y1);
}

u16 LinkTextColor() { return 0x001F; }

} // namespace

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

void Page::FreeBuffer() {
  std::vector<u32>().swap(storage);
  SyncBufferAlias();
}

void Page::Draw(Text *ts) {
  const bool saved_auto_wrap = ts->IsAutoWrapEnabled();
  const bool saved_clip_to_content = ts->IsClipToContentEnabled();
  const u8 saved_pixel_size = ts->GetPixelSize();
  // Reflowed page buffers already carry explicit line breaks/wrap decisions.
  // Runtime per-glyph wrapping in TextRenderer breaks RTL line anchoring.
  ts->SetAutoWrapEnabled(false);

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
  u8 underline_style = UNDERLINE_STYLE_SOLID;
  bool overline = false;
  bool strikethrough = false;
  bool superscript = false;
  bool subscript = false;
  bool mono = false;
  bool link_active = false;
  u16 active_link_href_id = 0;
  int active_link_render_index = -1;
  rendered_inline_links_.clear();

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
      if (book) {
        if (ts->GetScreen() == ts->screenright)
          book->DrawBottomGradientBackground();
        else
          book->DrawTopGradientBackground();
        ts->MarkScreenDirty(ts->GetScreen());
      } else {
        ts->ClearScreen();
      }
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
  if (book) {
    book->DrawTopGradientBackground();
    ts->MarkScreenDirty(ts->screenleft);
  } else {
    ts->ClearScreen();
  }
  ts->SetScreen(ts->screenright);
  if (book) {
    book->DrawBottomGradientBackground();
    ts->MarkScreenDirty(ts->screenright);
  } else {
    ts->ClearScreen();
  }
  ts->SetScreen(first_screen);

  u16 i = 0;
  InlineImageContext next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
  bool rtl_paragraph = false;
  u32 rtl_line_px = 0;  // parse-time line width stashed by TEXT_RTL_LINE_PX
  bool in_preformatted_block = false;
  book_xml_css_style_utils::TextAlign paragraph_align = book_xml_css_style_utils::TextAlign::Left;
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
    } else if (c == TEXT_PARAGRAPH_LEFT) {
      paragraph_align = book_xml_css_style_utils::TextAlign::Left;
      i++;
      continue;
    } else if (c == TEXT_PARAGRAPH_CENTER) {
      paragraph_align = book_xml_css_style_utils::TextAlign::Center;
      i++;
      continue;
    } else if (c == TEXT_PARAGRAPH_RIGHT) {
      paragraph_align = book_xml_css_style_utils::TextAlign::Right;
      i++;
      continue;
    } else if (c == TEXT_LINK_START) {
      if (i + 1 < length) {
        active_link_href_id = (u16)buf[i + 1];
        link_active = active_link_href_id != 0;
        active_link_render_index = -1;
        if (link_active) {
          InlineLinkRenderEntry entry{};
          entry.href_id = active_link_href_id;
          entry.screen_index = on_first_screen ? 0 : 1;
          entry.bounds.x0 = 0;
          entry.bounds.y0 = 0;
          entry.bounds.x1 = 0;
          entry.bounds.y1 = 0;
          rendered_inline_links_.push_back(entry);
          active_link_render_index = (int)rendered_inline_links_.size() - 1;
        }
        i += 2;
      } else {
        i++;
      }
      continue;
    } else if (c == TEXT_LINK_END) {
      i++;
      link_active = false;
      active_link_href_id = 0;
      active_link_render_index = -1;
      ts->ClearTextColorOverride();
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
      if (text_render_layout_utils::WouldOverflowReadingScreen(
              ts->GetPenY(), ts->GetHeight(), ts->linespacing, maxHeight,
              currentBottomMargin)) {
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
          if (book) {
            if (ts->GetScreen() == ts->screenright)
              book->DrawBottomGradientBackground();
            else
              book->DrawTopGradientBackground();
            ts->MarkScreenDirty(ts->GetScreen());
          } else {
            ts->ClearScreen();
          }
          ts->InitPen();
          ts->linebegan = false;
        } else
        {
          break;
        }
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
    } else if (c == TEXT_UNDERLINE_ON) {
      i++;
      underline = true;
      underline_style = UNDERLINE_STYLE_SOLID;
    } else if (c == TEXT_UNDERLINE_OFF) {
      i++;
      underline = false;
      underline_style = UNDERLINE_STYLE_SOLID;
    } else if (c == TEXT_UNDERLINE_STYLE) {
      if (i + 1 < length)
        underline_style = (u8)buf[i + 1];
      i += (i + 1 < length) ? 2 : 1;
    } else if (c == TEXT_FONT_SIZE) {
      if (i + 1 < length)
        ts->SetPixelSize((u8)buf[i + 1]);
      i += (i + 1 < length) ? 2 : 1;
    } else if (c == TEXT_OVERLINE_ON) {
      i++;
      overline = true;
    } else if (c == TEXT_OVERLINE_OFF) {
      i++;
      overline = false;
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
    } else if (c == TEXT_PRE_ON) {
      i++;
      in_preformatted_block = true;
      ts->SetClipToContentEnabled(true);
    } else if (c == TEXT_PRE_OFF) {
      i++;
      in_preformatted_block = false;
      ts->SetClipToContentEnabled(saved_clip_to_content);
    } else if (c == TEXT_HR) {
      i++;
      const int x0 = ts->margin.left;
      const int x1 = ts->display.width - ts->margin.right;
      // Draw the rule slightly below centre of the line box so it sits between
      // the preceding and following text rather than within the ascender zone.
      const int y = std::max(ts->margin.top,
                             ts->GetPenY() - std::max(1, ts->GetHeight() / 3));
      ts->FillRect(x0, y, x1, y + 1, ts->GetFgColor());
      if (!ts->PrintNewLine()) {
        // Screen 0 is full; advance to screen 1 so that any content the
        // parser placed there is actually rendered, rather than stopping here.
        if (!advance_to_next_screen())
          break;
      }
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
        int current_screen = on_first_screen ? 0 : 1;
        book->PlanInlineImageLayout(ts, image_id, current_screen, ts->GetPenX(),
                                    ts->GetPenY(), ts->linebegan, next_image_context,
                                    &image_plan);
        next_image_context = INLINE_IMAGE_CONTEXT_DEFAULT;

        if (image_plan.advance_before) {
          if (!advance_to_next_screen()) {
            break;
          }
          current_screen = on_first_screen ? 0 : 1;
        }
        if (image_plan.line_break_before && ts->linebegan) {
          if (!ts->PrintNewLine()) {
            break;
          }
          ts->linebegan = false;
        }

        book->DrawInlineImage(ts, image_id, &image_plan, current_screen);

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
          {
            const text_render_layout_utils::ReadingScreenMetrics metrics =
                text_render_layout_utils::ResolveReadingScreenMetrics(
                    on_first_screen, first_is_left, leftBottomMargin,
                    rightBottomMargin);
            if (text_render_layout_utils::WouldOverflowReadingScreen(
                    ts->GetPenY(), ts->GetHeight(), ts->linespacing,
                    metrics.max_height, metrics.bottom_margin)) {
              if (!advance_to_next_screen()) {
                stop_page_draw = true;
              }
            }
          }
          break;

        case INLINE_IMAGE_LAYOUT_PAGE:
        default:
          if (!advance_to_next_screen()) {
            stop_page_draw = true;
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
          line_width = page_alignment_utils::MeasureAlignedLineWidth(
              buf, length, (size_t)(i - 1), ts->bold, ts->italic, mono,
              [](u32 codepoint, unsigned char style, void *ctx) -> int {
                return ((Text *)ctx)->GetAdvance(codepoint, style);
              },
              ts);
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
      } else if (ts->GetPenX() == ts->margin.left &&
                 (paragraph_align == book_xml_css_style_utils::TextAlign::Center ||
                  paragraph_align == book_xml_css_style_utils::TextAlign::Right)) {
        int line_width = page_alignment_utils::MeasureAlignedLineWidth(
            buf, length, (size_t)(i - 1), ts->bold, ts->italic, mono,
            [](u32 codepoint, unsigned char style, void *ctx) -> int {
              return ((Text *)ctx)->GetAdvance(codepoint, style);
            },
            ts);
        int available_width = ts->display.width - ts->margin.left - ts->margin.right;
        int x_offset = 0;
        if (paragraph_align == book_xml_css_style_utils::TextAlign::Center) {
          x_offset = (available_width - line_width) / 2;
        } else if (paragraph_align == book_xml_css_style_utils::TextAlign::Right) {
          x_offset = available_width - line_width;
        }
        if (x_offset < 0)
          x_offset = 0;
        ts->SetPen((u16)(ts->margin.left + x_offset), ts->GetPenY());
      }

      const int glyph_x0 = (int)ts->GetPenX();
      const int base_pen_y = (int)ts->GetPenY();
      if (link_active)
        ts->SetTextColorOverride(LinkTextColor());
      else
        ts->ClearTextColorOverride();
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
      if (link_active && active_link_render_index >= 0 &&
          active_link_render_index < (int)rendered_inline_links_.size()) {
        InlineLinkRenderEntry &entry =
            rendered_inline_links_[(size_t)active_link_render_index];
        entry.screen_index = on_first_screen ? 0 : 1;
        ExpandLinkBounds(&entry.bounds, glyph_x0, base_pen_y - ts->GetHeight(),
                         glyph_x1, base_pen_y + 2);
      }
      if (superscript || subscript)
        ts->SetPen((u16)glyph_x1, (u16)base_pen_y);
      const int baseline_y = (int)ts->GetPenY();
      const u16 deco_color = ts->GetFgColor();
      if (glyph_x1 > glyph_x0) {
        if (overline) {
          const int y = baseline_y - ts->GetHeight() + 2;
          DrawSolidDecoration(ts, glyph_x0, glyph_x1, y, deco_color);
        }
        if (underline) {
          const int y = baseline_y + 1;
          DrawPatternedUnderline(ts, glyph_x0, glyph_x1, y, deco_color,
                                 underline_style);
        }
        if (strikethrough) {
          const int y = baseline_y - std::max(2, ts->GetHeight() / 3);
          if (y >= 0)
            ts->FillRect((u16)glyph_x0, (u16)y, (u16)glyph_x1, (u16)(y + 1),
                         deco_color);
        }
      }

      ts->linebegan = true;
    }
  }

  if (in_preformatted_block)
    ts->SetClipToContentEnabled(saved_clip_to_content);
  ts->ClearTextColorOverride();
  if (book) {
    const int focused_index = book->GetFocusedInlineLinkIndex();
    if (focused_index >= 0 &&
        focused_index < (int)rendered_inline_links_.size()) {
      const InlineLinkRenderEntry &entry =
          rendered_inline_links_[(size_t)focused_index];
      if (inline_link_utils::IsValidRect(entry.bounds)) {
        u16 *saved_screen = ts->GetScreen();
        u16 *target = (entry.screen_index == 0) ? first_screen : second_screen;
        ts->SetScreen(target);
        const int x0 = std::max(0, entry.bounds.x0 - 1);
        const int y0 = std::max(0, entry.bounds.y0 - 1);
        const int x1 = std::min((int)ts->display.width, entry.bounds.x1 + 1);
        const int max_y = (target == ts->screenleft) ? 400 : 320;
        const int y1 = std::min(max_y, entry.bounds.y1 + 1);
        const u16 focus_color = 0xF800;
        ts->FillRect((u16)x0, (u16)y0, (u16)x1, (u16)(y0 + 1), focus_color);
        ts->FillRect((u16)x0, (u16)(y1 - 1), (u16)x1, (u16)y1, focus_color);
        ts->FillRect((u16)x0, (u16)y0, (u16)(x0 + 1), (u16)y1, focus_color);
        ts->FillRect((u16)(x1 - 1), (u16)y0, (u16)x1, (u16)y1, focus_color);
        ts->SetScreen(saved_screen);
      }
    }
  }
  ts->SetPixelSize(saved_pixel_size);
  DrawNumber(ts, second_screen);
#ifdef OFFSCREEN
  ts->SetScreen(second_screen);
  ts->CopyScreen(ts->offscreen, ts->screen);
  ts->SetScreen(pushscreen);
#endif
  ts->SetAutoWrapEnabled(saved_auto_wrap);
  ts->SetClipToContentEnabled(saved_clip_to_content);
  ts->SetPixelSize(saved_pixel_size);
  ts->margin.bottom = savedBottomMargin;
}

void Page::DrawNumber(Text *ts, u16 *number_screen) {
  //! Draw page number on current screen.
  char msg[64];

  // Find out if the page is bookmarked or not
  bool isBookmark = false;
  u16 pagecurrent = book->GetPosition();
  u16 pagecount = book->GetPageCount();
  std::list<u16> &bookmarks = book->GetBookmarks();
  for (std::list<u16>::iterator i = bookmarks.begin(); i != bookmarks.end();
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
