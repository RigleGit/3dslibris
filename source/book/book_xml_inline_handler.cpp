/*
    3dslibris - book_xml_inline_handler.cpp

    Inline element start/end handling and CSS-based inline styling.
    Extracted from book_xml_parser.cpp.
*/

#include "book/book_xml_inline_handler.h"

#include "book/book.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_element_style.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_list_utils.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_screen_advance.h"
#include "book/epub_css_class_map.h"
#include "parse.h"
#include "settings/prefs.h"
#include "shared/text_render_layout_utils.h"
#include "ui/text.h"

#include <string.h>

namespace book_xml_inline_handler {

bool HandleNamedInlineElementStart(parsedata_t *p, Text *ts, const char *el,
                                    const char **attr,
                                    const InlineHandlerFns &fns) {
  if (!p || !ts || !el)
    return false;

  if (!strcmp(el, "strong") || !strcmp(el, "b")) {
    parse_push(p, TAG_STRONG);
    parse_append_page_byte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    ts->SetStyle(book_xml_parser_style_utils::ResolveParsedTextStyle(
        p->bold, p->italic, p->mono));
    return true;
  }
  if (!strcmp(el, "em") || !strcmp(el, "i")) {
    parse_push(p, TAG_EM);
    parse_append_page_byte(p, TEXT_ITALIC_ON);
    p->italic = true;
    ts->SetStyle(book_xml_parser_style_utils::ResolveParsedTextStyle(
        p->bold, p->italic, p->mono));
    return true;
  }
  if (!strcmp(el, "u") || !strcmp(el, "ins")) {
    parse_push(p, TAG_UNDERLINE);
    if (!p->underline) {
      parse_append_page_byte(p, TEXT_UNDERLINE_ON);
      p->underline = true;
      p->underline_style = UNDERLINE_STYLE_SOLID;
      book_xml_parser_style_utils::EmitUnderlineStyleMarker(p, p->underline_style);
    }
    return true;
  }
  if (!strcmp(el, "strike") || !strcmp(el, "s") || !strcmp(el, "del")) {
    parse_push(p, TAG_STRIKETHROUGH);
    if (!p->strikethrough) {
      parse_append_page_byte(p, TEXT_STRIKETHROUGH_ON);
      p->strikethrough = true;
    }
    return true;
  }
  if (!strcmp(el, "sup")) {
    parse_push(p, TAG_SUPERSCRIPT);
    if (!p->superscript) {
      parse_append_page_byte(p, TEXT_SUPERSCRIPT_ON);
      p->superscript = true;
    }
    return true;
  }
  if (!strcmp(el, "sub")) {
    parse_push(p, TAG_SUBSCRIPT);
    if (!p->subscript) {
      parse_append_page_byte(p, TEXT_SUBSCRIPT_ON);
      p->subscript = true;
    }
    return true;
  }
  if (!strcmp(el, "code") || !strcmp(el, "tt") ||
      !strcmp(el, "kbd") || !strcmp(el, "samp")) {
    parse_push(p, TAG_CODE);
    if (!p->mono) {
      parse_append_page_byte(p, TEXT_MONO_ON);
      p->mono = true;
      ts->SetStyle(book_xml_parser_style_utils::ResolveParsedTextStyle(
          p->bold, p->italic, p->mono));
    }
    return true;
  }
  if (!strcmp(el, "ruby")) {
    parse_push(p, TAG_RUBY);
    return true;
  }
  if (!strcmp(el, "rp")) {
    // <rp> provides fallback parens for non-ruby renderers; we add our own
    // around <rt>, so suppress <rp> content entirely.
    parse_push(p, TAG_RP);
    if (p->stacksize > 0)
      p->style_hidden_stack[p->stacksize - 1] = true;
    return true;
  }
  if (!strcmp(el, "rt")) {
    parse_push(p, TAG_RT);
    if (!book_xml_inline_state::HasActiveStackHiddenStyle(p)) {
      // Render annotation as (text) at ~75% size.
      fns.emit_chardata(p, "(", 1);
      const u8 current = (u8)(p->stacksize - 1);
      const u8 saved_px = ts->GetPixelSize();
      const u8 small_px = (u8)book_xml_parser_style_utils::ClampInlineFontSize(
          p->base_font_size_px, (int)(saved_px * 3 / 4));
      if (small_px != saved_px) {
        p->style_font_size_stack[current] = small_px;
        p->style_font_size_restore_stack[current] = saved_px;
        ts->SetPixelSize(small_px);
        parse_append_page_byte(p, TEXT_FONT_SIZE);
        parse_append_page_byte(p, (u32)small_px);
      }
    }
    return true;
  }
  return false;
}

void HandleCssInlineStylingStart(
    parsedata_t *p, Text *ts, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !ts || !el)
    return;
  // CSS-based emphasis fallback for EPUBs that do not use semantic tags.
  if (!parse_in(p, TAG_BODY) || p->stacksize == 0)
    return;

  bool style_bold = false;
  bool style_italic = false;
  bool style_underline = false;
  u8 style_underline_style = UNDERLINE_STYLE_SOLID;
  bool style_overline = false;
  bool style_strikethrough = false;
  bool style_superscript = false;
  bool style_subscript = false;
  bool style_no_underline = false;
  bool style_reset_bold = false;
  bool style_reset_italic = false;
  bool style_hidden = false;
  book_xml_css_resolver::ParseElementStyleFlags(
      attr, &style_bold, &style_italic, &style_underline,
      reinterpret_cast<uint8_t *>(&style_underline_style),
      &style_overline, &style_strikethrough,
      &style_superscript, &style_subscript,
      &style_no_underline, &style_reset_bold, &style_reset_italic);
  // Use pre-resolved CSS class properties from elem_css.
  if (!style_superscript) style_superscript = elem_css.superscript;
  if (!style_subscript) style_subscript = elem_css.subscript;
  if (!style_no_underline) style_no_underline = elem_css.no_underline;
  if (!style_reset_bold) style_reset_bold = elem_css.reset_bold;
  if (!style_reset_italic) style_reset_italic = elem_css.reset_italic;
  if (!style_bold) style_bold = elem_css.force_bold;
  if (!style_italic) style_italic = elem_css.force_italic;
  book_xml_css_resolver::ParseElementHiddenFlags(attr, &style_hidden);
  if (elem_css.is_display_none)
    style_hidden = true;

  const u8 current = (u8)(p->stacksize - 1);
  book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  p->style_bold_stack[current] = style_bold;
  p->style_italic_stack[current] = style_italic;
  p->style_underline_stack[current] = style_underline;
  p->style_underline_style_stack[current] = style_underline_style;
  p->style_overline_stack[current] = style_overline;
  p->style_strikethrough_stack[current] = style_strikethrough;
  p->style_superscript_stack[current] = style_superscript;
  p->style_subscript_stack[current] = style_subscript;
  p->style_hidden_stack[current] = style_hidden;
  p->style_no_underline_stack[current] = style_no_underline;
  p->style_reset_bold_stack[current] = style_reset_bold;
  p->style_reset_italic_stack[current] = style_reset_italic;
  p->style_text_transform_stack[current] =
      book_xml_element_style::ParseElementTextTransform(attr, elem_css);
  p->style_white_space_stack[current] =
      book_xml_element_style::ParseElementWhiteSpace(attr, elem_css);

  // Font-size: <small>/<big> and CSS font-size (headings manage their own)
  {
    const bool is_heading_el =
        (el[0] == 'h' && el[1] >= '1' && el[1] <= '6' && !el[2]);
    u8 new_font_px = 0;
    if (!is_heading_el) {
      book_xml_css_style_utils::FontSizeSpec spec;
      bool has_spec = false;
      // Always honor publisher CSS font-size. Absolute px values are scaled
      // relative to the 16px CSS baseline so the user's font-size preference
      // acts as a global scale factor on all publisher proportions.
      has_spec = book_xml_css_style_utils::TryParseFontSize(
          book_xml_css_resolver::ExtractStyleAttr(attr).c_str(), &spec);
      if (!has_spec &&
          elem_css.font_size.unit !=
              book_xml_css_style_utils::FontSizeSpec::Unit::None) {
        spec = elem_css.font_size;
        has_spec = true;
      }
      // <small>/<big> semantic elements override if no CSS font-size present.
      if (!has_spec) {
        if (!strcmp(el, "small")) {
          spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Smaller;
          has_spec = true;
        } else if (!strcmp(el, "big")) {
          spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Larger;
          has_spec = true;
        }
      }
      if (has_spec &&
          spec.unit != book_xml_css_style_utils::FontSizeSpec::Unit::None) {
        if (p->base_font_size_px == 0)
          p->base_font_size_px = ts->GetPixelSize();
        const int px = book_xml_css_style_utils::ResolveFontSizePx(
            spec, (int)ts->GetPixelSize());
        new_font_px = (u8)book_xml_parser_style_utils::ClampInlineFontSize(
            p->base_font_size_px, px);
        if (new_font_px == ts->GetPixelSize())
          new_font_px = 0;
      }
    }
    if (new_font_px) {
      p->style_font_size_stack[current] = new_font_px;
      p->style_font_size_restore_stack[current] = ts->GetPixelSize();
      ts->SetPixelSize(new_font_px);
      parse_append_page_byte(p, TEXT_FONT_SIZE);
      parse_append_page_byte(p, new_font_px);
    } else {
      p->style_font_size_stack[current] = 0;
      p->style_font_size_restore_stack[current] = 0;
    }
  }

  bool style_changed = false;
  if (style_bold && !style_reset_bold && !p->bold) {
    parse_append_page_byte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    style_changed = true;
  }
  if (style_reset_bold && p->bold) {
    parse_append_page_byte(p, TEXT_BOLD_OFF);
    p->bold = false;
    style_changed = true;
  }
  if (style_italic && !style_reset_italic && !p->italic) {
    parse_append_page_byte(p, TEXT_ITALIC_ON);
    p->italic = true;
    style_changed = true;
  }
  if (style_reset_italic && p->italic) {
    parse_append_page_byte(p, TEXT_ITALIC_OFF);
    p->italic = false;
    style_changed = true;
  }
  if (style_no_underline && p->underline) {
    parse_append_page_byte(p, TEXT_UNDERLINE_OFF);
    p->underline = false;
    p->underline_style = UNDERLINE_STYLE_SOLID;
    style_changed = true;
  }
  if (style_underline && !style_no_underline && !p->underline) {
    parse_append_page_byte(p, TEXT_UNDERLINE_ON);
    p->underline = true;
    p->underline_style = style_underline_style;
    book_xml_parser_style_utils::EmitUnderlineStyleMarker(p, p->underline_style);
    style_changed = true;
  } else if (style_underline && p->underline &&
             p->underline_style != style_underline_style) {
    p->underline_style = style_underline_style;
    book_xml_parser_style_utils::EmitUnderlineStyleMarker(p, p->underline_style);
  }
  if (style_overline && !p->overline) {
    parse_append_page_byte(p, TEXT_OVERLINE_ON);
    p->overline = true;
    style_changed = true;
  }
  if (style_strikethrough && !p->strikethrough) {
    parse_append_page_byte(p, TEXT_STRIKETHROUGH_ON);
    p->strikethrough = true;
    style_changed = true;
  }
  if (style_superscript && !p->superscript) {
    parse_append_page_byte(p, TEXT_SUPERSCRIPT_ON);
    p->superscript = true;
    style_changed = true;
  }
  if (style_subscript && !p->subscript) {
    parse_append_page_byte(p, TEXT_SUBSCRIPT_ON);
    p->subscript = true;
    style_changed = true;
  }
  if (style_changed)
    ts->SetStyle(book_xml_parser_style_utils::ResolveParsedTextStyle(
        p->bold, p->italic, p->mono));
}

void SyncInlineStyleAfterPop(parsedata_t *p, Text *ts) {
  if (!p || !ts || !p->book)
    return;

  const bool any_reset_bold = book_xml_inline_state::HasActiveStackResetBoldStyle(p);
  const bool any_reset_italic = book_xml_inline_state::HasActiveStackResetItalicStyle(p);
  const bool any_no_underline = book_xml_inline_state::HasActiveStackNoUnderlineStyle(p);
  const bool want_bold =
      !any_reset_bold &&
      (parse_in(p, TAG_STRONG) || parse_in(p, TAG_H1) || parse_in(p, TAG_H2) ||
       parse_in(p, TAG_H3) || parse_in(p, TAG_H4) || parse_in(p, TAG_H5) ||
       parse_in(p, TAG_H6) || book_xml_inline_state::HasActiveStackBoldStyle(p));
  const bool want_italic =
      !any_reset_italic &&
      (parse_in(p, TAG_EM) || book_xml_inline_state::HasActiveStackItalicStyle(p));
  const bool want_underline =
      !any_no_underline &&
      (parse_in(p, TAG_UNDERLINE) ||
       book_xml_inline_state::HasActiveStackUnderlineStyle(p));
  const u8 want_underline_style =
      want_underline
          ? book_xml_inline_state::ResolveActiveUnderlineStyle(p)
          : UNDERLINE_STYLE_SOLID;
  const bool want_overline = book_xml_inline_state::HasActiveStackOverlineStyle(p);
  const bool want_strikethrough =
      parse_in(p, TAG_STRIKETHROUGH) ||
      book_xml_inline_state::HasActiveStackStrikethroughStyle(p);
  const bool want_superscript =
      parse_in(p, TAG_SUPERSCRIPT) ||
      book_xml_inline_state::HasActiveStackSuperscriptStyle(p);
  const bool want_subscript =
      parse_in(p, TAG_SUBSCRIPT) ||
      book_xml_inline_state::HasActiveStackSubscriptStyle(p);
  const bool want_mono =
      parse_in(p, TAG_CODE) || parse_in(p, TAG_PRE) ||
      book_xml_inline_state::HasActiveStackMonoStyle(p);

  const bool needs_style_sync =
      p->bold != want_bold || p->italic != want_italic ||
      p->underline != want_underline ||
      (want_underline && p->underline_style != want_underline_style) ||
      p->overline != want_overline ||
      p->strikethrough != want_strikethrough ||
      p->superscript != want_superscript || p->subscript != want_subscript ||
      p->mono != want_mono;

  if (needs_style_sync) {
    book_xml_inline_state::QueueDeferredStyleSync(
        p, want_bold, want_italic, want_underline, want_underline_style,
        want_overline, want_strikethrough, want_superscript, want_subscript,
        want_mono);
    book_xml_flow_emission::ApplyDeferredStyleSync(p, ts);
  }

  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(
              ts->margin.bottom));
  if (!text_render_layout_utils::CurrentLineFitsScreen(
        p->pen.y, ts->GetHeight(), ts->linespacing,
        metrics.max_height, metrics.bottom_margin)) {
    book_xml_screen_advance::AdvanceParsedScreen(p);
  }
}

} // namespace book_xml_inline_handler
