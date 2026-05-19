/*
    3dslibris - book_xml_element_style.cpp

    Per-element CSS/style attribute parsing and block layout configuration.
    Extracted from book_xml_parser.cpp.
*/

#include "book/book_xml_element_style.h"

#include "book/book.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_screen_advance.h"
#include "book/epub_css_class_map.h"
#include "parse.h"
#include "shared/debug_log.h"
#include "shared/text_render_layout_utils.h"
#include "ui/text.h"

#include <string>

#ifndef BLOCK_BOUNDARY_TRACE
#define BLOCK_BOUNDARY_TRACE 0
#endif

namespace {

static bool EqualsAsciiNoCase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static void AppendParagraphAlignMarker(
    parsedata_t *p, book_xml_css_style_utils::TextAlign align) {
  if (!p)
    return;
  if (align == book_xml_css_style_utils::TextAlign::Center) {
    parse_append_page_byte(p, TEXT_PARAGRAPH_CENTER);
  } else if (align == book_xml_css_style_utils::TextAlign::Right) {
    parse_append_page_byte(p, TEXT_PARAGRAPH_RIGHT);
  } else {
    parse_append_page_byte(p, TEXT_PARAGRAPH_LEFT);
  }
}

} // namespace

namespace book_xml_element_style {

book_xml_css_style_utils::ClearMode ParseElementClear(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::ClearMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        ClearMode mode = ClearMode::None;
        if (book_xml_css_style_utils::TryParseClear(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_clear ? elem_css.clear_mode : ClearMode::None;
}

book_xml_css_style_utils::FloatMode ParseElementFloat(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::FloatMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        FloatMode mode = FloatMode::None;
        if (book_xml_css_style_utils::TryParseFloat(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_float ? elem_css.float_mode : FloatMode::None;
}

u8 ParseElementTextTransform(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::TextTransform;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const TextTransform tt =
            book_xml_css_style_utils::ParseTextTransform(attr[i + 1]);
        if (tt != TextTransform::None)
          return (u8)tt;
      }
    }
  }
  return elem_css.has_text_transform ? (u8)elem_css.text_transform : 0;
}

u8 ParseElementWhiteSpace(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::WhiteSpaceMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        WhiteSpaceMode mode = WhiteSpaceMode::Normal;
        if (book_xml_css_style_utils::TryParseWhiteSpace(attr[i + 1], &mode))
          return (u8)mode + 1;
      }
    }
  }
  return elem_css.has_white_space ? (u8)elem_css.white_space + 1 : 0;
}

void ApplyElementBlockMargins(
    parsedata_t *p, Text *ts, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !ts)
    return;
  using book_xml_css_style_utils::MarginTopResult;
  const int inherited_left = parse_current_block_margin_left(p);
  const int inherited_right = parse_current_block_margin_right(p);
  int effective_left = inherited_left;
  int effective_right = inherited_right;

  MarginTopResult ml, mr;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const auto r = book_xml_css_style_utils::ParseMarginLeft(attr[i + 1]);
        if (r.unit != MarginTopResult::Unit::None)
          ml = r;
        const auto r2 = book_xml_css_style_utils::ParseMarginRight(attr[i + 1]);
        if (r2.unit != MarginTopResult::Unit::None)
          mr = r2;
        break;
      }
    }
  }
  if (ml.unit == MarginTopResult::Unit::None &&
      elem_css.margin_left.unit != MarginTopResult::Unit::None)
    ml = elem_css.margin_left;
  if (mr.unit == MarginTopResult::Unit::None &&
      elem_css.margin_right.unit != MarginTopResult::Unit::None)
    mr = elem_css.margin_right;

  if (ml.unit != MarginTopResult::Unit::None)
    effective_left +=
        book_xml_css_style_utils::ResolveHorizontalMarginPx(ml, ts->display.width, (int)ts->GetPixelSize());
  if (mr.unit != MarginTopResult::Unit::None)
    effective_right +=
        book_xml_css_style_utils::ResolveHorizontalMarginPx(mr, ts->display.width, (int)ts->GetPixelSize());
  parse_set_current_block_margins(p, effective_left, effective_right);
}

book_xml_css_style_utils::MarginTopResult ParseElementMarginTopWithClass(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  const book_xml_css_style_utils::MarginTopResult from_style =
      book_xml_css_resolver::ParseElementMarginTopPx(attr);
  if (from_style.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
    return from_style;
  return elem_css.margin_top;
}

void ConfigureBlockTextAlign(
    parsedata_t *p, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !el || p->stacksize == 0)
    return;
  const std::string style_attr = book_xml_css_resolver::ExtractStyleAttr(attr);
  const bool can_carry =
      book_xml_css_resolver::ElementCanCarryBlockTextAlign(el, style_attr) ||
      elem_css.is_display_block;
  if (!can_carry)
    return;

  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  bool has_align =
      book_xml_css_style_utils::TryParseTextAlign(style_attr.c_str(), &align);
  if (!has_align && elem_css.has_text_align) {
    has_align = true;
    align = elem_css.text_align;
  }
  if (!has_align)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  p->block_text_align_stack[current] = true;
  p->block_text_align_value_stack[current] = (u8)align;
  AppendParagraphAlignMarker(p, align);
}

void EnsureBlockBoundaryBeforeBlockStart(parsedata_t *p, const char *tag,
                                          const char *reason) {
  if (!p || !p->ts || !p->book)
    return;
#if defined(DSLIBRIS_DEBUG) && BLOCK_BOUNDARY_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "EnsureBoundary ENTER[%s/%s] pbb=%d pbl=%d from_css=%d pen_y=%d lb=%d scr=%d vis=%d buflen=%d",
    tag ? tag : "?", reason ? reason : "?",
    p->pending_block_break ? 1 : 0,
    p->pending_block_spacing_lf,
    p->pending_block_spacing_from_css ? 1 : 0,
    p->pen.y, p->linebegan ? 1 : 0, p->screen,
    p->current_screen_has_drawable_content ? 1 : 0, p->buflen);
#endif

  if (book_xml_screen_advance::IsCurrentReadingScreenVisuallyEmpty(p)) {
    p->pending_block_break = false;
    return;
  }

  const int line_step = p->ts->GetHeight() + p->ts->linespacing;

  auto advance_or_linefeed = [&]() {
    if (line_step <= 0) {
      book_xml_screen_advance::Linefeed(p);
      return;
    }

    const int compact_bottom =
        p->ts->margin.bottom < 16 ? p->ts->margin.bottom : 16;
    const text_render_layout_utils::ReadingScreenMetrics metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0,
            p->screen,
            p->ts->margin.bottom,
            compact_bottom);

    const int next_y = p->pen.y + line_step;
    const bool next_line_fits =
        text_render_layout_utils::CurrentLineFitsScreen(
            next_y,
            p->ts->GetHeight(),
            p->ts->linespacing,
            metrics.max_height,
            metrics.bottom_margin);

    if (next_line_fits) {
      book_xml_screen_advance::Linefeed(p);
    } else {
      book_xml_screen_advance::AdvanceParsedScreen(p);
    }
  };

  if (p->pending_block_break) {
#if defined(DSLIBRIS_DEBUG) && BLOCK_BOUNDARY_TRACE
    DBG_LOGF(p->book->GetStatusReporter(),
      "EnsureBoundary PBB-path[%s] pen_y=%d lb=%d scr=%d",
      tag ? tag : "?", p->pen.y, p->linebegan ? 1 : 0, p->screen);
#endif
    advance_or_linefeed();
    p->pending_block_break = false;
    return;
  }

  const bool buffer_already_broken =
      p->buflen > 0 && p->buf[p->buflen - 1] == '\n';

  const bool needs_boundary =
      p->linebegan ||
      (p->buflen > 0 &&
       !buffer_already_broken &&
       !book_xml_screen_advance::IsCurrentReadingScreenVisuallyEmpty(p));

#if defined(DSLIBRIS_DEBUG) && BLOCK_BOUNDARY_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "EnsureBoundary needs=%d lb=%d buf_broken=%d pen_y=%d scr=%d",
    needs_boundary ? 1 : 0,
    p->linebegan ? 1 : 0, buffer_already_broken ? 1 : 0,
    p->pen.y, p->screen);
#endif

  if (!needs_boundary)
    return;

  advance_or_linefeed();
}

} // namespace book_xml_element_style
