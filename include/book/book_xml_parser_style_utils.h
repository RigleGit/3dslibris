#pragma once

#include "book/book_xml_css_style_utils.h"
#include "parse.h"
#include "screen_constants.h"
#include "shared/text_token_constants.h"

#include <algorithm>
#include <math.h>

namespace book_xml_parser_style_utils {

inline int ComputeHeadingFontSize(
    int base_px, int heading_level, const std::string &style_attr,
    const std::string &class_attr,
    const epub_css_class_map::CssClassMap &class_map);

inline void EmitUnderlineStyleMarker(parsedata_t *p, u8 underline_style) {
  if (!p)
    return;
  parse_append_page_byte(p, TEXT_UNDERLINE_STYLE);
  parse_append_page_byte(p, underline_style);
}

inline u8 ResolveParsedTextStyle(bool bold, bool italic, bool mono) {
  if (mono && bold && italic)
    return TEXT_STYLE_MONO_BOLDITALIC;
  if (mono && bold)
    return TEXT_STYLE_MONO_BOLD;
  if (mono && italic)
    return TEXT_STYLE_MONO_ITALIC;
  if (mono)
    return TEXT_STYLE_MONO;
  if (bold && italic)
    return TEXT_STYLE_BOLDITALIC;
  if (bold)
    return TEXT_STYLE_BOLD;
  if (italic)
    return TEXT_STYLE_ITALIC;
  return TEXT_STYLE_REGULAR;
}

inline void RestoreParsedParagraphAlignmentMarker(parsedata_t *p) {
  if (!p)
    return;

  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  bool have_align = false;
  bool in_alignable_block = false;

  if (parse_in(p, TAG_P)) {
    in_alignable_block = true;
    have_align = book_xml_css_style_utils::TryParseTextAlign(
        p->last_p_style.c_str(), &align);
    if (!have_align) {
      have_align = epub_css_class_map::LookupTextAlignForClassAttr(
          p->last_p_class, p->css_class_map, &align);
    }
  } else if (parse_in(p, TAG_H1)) {
    in_alignable_block = true;
    have_align = book_xml_css_style_utils::TryParseTextAlign(
        p->last_h1_style.c_str(), &align);
    if (!have_align) {
      have_align = epub_css_class_map::LookupTextAlignForClassAttr(
          p->last_h1_class, p->css_class_map, &align);
    }
  } else if (parse_in(p, TAG_H2)) {
    in_alignable_block = true;
    have_align = book_xml_css_style_utils::TryParseTextAlign(
        p->last_h2_style.c_str(), &align);
    if (!have_align) {
      have_align = epub_css_class_map::LookupTextAlignForClassAttr(
          p->last_h2_class, p->css_class_map, &align);
    }
  } else if (parse_in(p, TAG_H3) || parse_in(p, TAG_H4) || parse_in(p, TAG_H5) ||
             parse_in(p, TAG_H6)) {
    in_alignable_block = true;
    have_align = book_xml_css_style_utils::TryParseTextAlign(
        p->last_h_style.c_str(), &align);
    if (!have_align) {
      have_align = epub_css_class_map::LookupTextAlignForClassAttr(
          p->last_h_class, p->css_class_map, &align);
    }
  } else {
    for (int i = (int)p->stacksize - 1; i >= 0; --i) {
      if (!p->block_text_align_stack[i])
        continue;
      in_alignable_block = true;
      have_align = true;
      align = (book_xml_css_style_utils::TextAlign)
                  p->block_text_align_value_stack[i];
      break;
    }
  }

  if (in_alignable_block && !have_align) {
    for (int i = (int)p->stacksize - 1; i >= 0; --i) {
      if (!p->block_text_align_stack[i])
        continue;
      have_align = true;
      align = (book_xml_css_style_utils::TextAlign)
                  p->block_text_align_value_stack[i];
      break;
    }
  }

  if (!in_alignable_block)
    return;

  if (!have_align || align == book_xml_css_style_utils::TextAlign::Left) {
    parse_append_page_byte(p, TEXT_PARAGRAPH_LEFT);
  } else if (align == book_xml_css_style_utils::TextAlign::Center) {
    parse_append_page_byte(p, TEXT_PARAGRAPH_CENTER);
  } else if (align == book_xml_css_style_utils::TextAlign::Right) {
    parse_append_page_byte(p, TEXT_PARAGRAPH_RIGHT);
  } else {
    parse_append_page_byte(p, TEXT_PARAGRAPH_LEFT);
  }
}

inline int FindActiveHeadingStackIndex(const parsedata_t *p, int *heading_level) {
  if (!p)
    return -1;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    switch (p->stack[i]) {
    case TAG_H1:
      if (heading_level)
        *heading_level = 1;
      return i;
    case TAG_H2:
      if (heading_level)
        *heading_level = 2;
      return i;
    case TAG_H3:
      if (heading_level)
        *heading_level = 3;
      return i;
    case TAG_H4:
      if (heading_level)
        *heading_level = 4;
      return i;
    case TAG_H5:
      if (heading_level)
        *heading_level = 5;
      return i;
    case TAG_H6:
      if (heading_level)
        *heading_level = 6;
      return i;
    default:
      break;
    }
  }
  return -1;
}

inline void RestoreParsedHeadingFontSizeMarker(parsedata_t *p) {
  if (!p)
    return;

  int heading_level = 0;
  const int heading_index = FindActiveHeadingStackIndex(p, &heading_level);
  if (heading_index < 0 || heading_index >= 32 ||
      !p->heading_font_size_emitted_stack[heading_index])
    return;

  int heading_px = p->heading_saved_font_size_stack[heading_index];
  if (heading_level == 1) {
    heading_px = ComputeHeadingFontSize(
        p->heading_saved_font_size_stack[heading_index], heading_level,
        p->last_h1_style, p->last_h1_class, p->css_class_map);
  } else if (heading_level == 2) {
    heading_px = ComputeHeadingFontSize(
        p->heading_saved_font_size_stack[heading_index], heading_level,
        p->last_h2_style, p->last_h2_class, p->css_class_map);
  } else {
    heading_px = ComputeHeadingFontSize(
        p->heading_saved_font_size_stack[heading_index], heading_level,
        p->last_h_style, p->last_h_class, p->css_class_map);
  }

  if (heading_px != p->heading_saved_font_size_stack[heading_index]) {
    parse_append_page_byte(p, TEXT_FONT_SIZE);
    parse_append_page_byte(p, (u32)heading_px);
  }
}

inline void RestoreParsedInlineFontSizeMarker(parsedata_t *p) {
  if (!p)
    return;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->style_font_size_stack[i]) {
      parse_append_page_byte(p, TEXT_FONT_SIZE);
      parse_append_page_byte(p, (u32)p->style_font_size_stack[i]);
      return;
    }
  }
}

inline void RestoreParsedStyleMarkers(parsedata_t *p) {
  if (!p)
    return;
  RestoreParsedParagraphAlignmentMarker(p);
  RestoreParsedHeadingFontSizeMarker(p);
  RestoreParsedInlineFontSizeMarker(p);
  if (parse_in(p, TAG_PRE))
    parse_append_page_byte(p, TEXT_PRE_ON);
  if (p->superscript)
    parse_append_page_byte(p, TEXT_SUPERSCRIPT_ON);
  if (p->subscript)
    parse_append_page_byte(p, TEXT_SUBSCRIPT_ON);
  if (p->strikethrough)
    parse_append_page_byte(p, TEXT_STRIKETHROUGH_ON);
  if (p->overline)
    parse_append_page_byte(p, TEXT_OVERLINE_ON);
  if (p->underline) {
    parse_append_page_byte(p, TEXT_UNDERLINE_ON);
    EmitUnderlineStyleMarker(p, p->underline_style);
  }
  if (p->italic)
    parse_append_page_byte(p, TEXT_ITALIC_ON);
  if (p->bold)
    parse_append_page_byte(p, TEXT_BOLD_ON);
  if (p->mono)
    parse_append_page_byte(p, TEXT_MONO_ON);
}

inline int ResolveCssMarginLinefeeds(
    const book_xml_css_style_utils::MarginTopResult &m, int line_h) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  if (m.unit == Unit::None || line_h <= 0)
    return 0;
  int px = m.value;
  if (m.unit == Unit::Percent)
    px = (m.value * PAGE_WIDTH) / 100;
  if (m.negative || px <= 0)
    return 0;
  const int linefeeds = (px + line_h - 1) / line_h;
  return std::max(1, std::min(linefeeds, 4));
}

inline int ClampResolvedBlockLinefeeds(int linefeeds) {
  return std::max(0, std::min(linefeeds, 6));
}

inline int ResolveBlockTopLinefeeds(
    int default_lf, const book_xml_css_style_utils::MarginTopResult &m,
    int line_h) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  if (m.unit == Unit::None)
    return ClampResolvedBlockLinefeeds(default_lf);
  if (m.negative || (m.unit != Unit::None && m.value == 0))
    return 0;
  const int css_lf = ResolveCssMarginLinefeeds(m, line_h);
  return ClampResolvedBlockLinefeeds(std::max(css_lf, default_lf + 1));
}

inline int ResolveBlockBottomLinefeeds(
    int default_lf, const book_xml_css_style_utils::MarginTopResult &m,
    int line_h) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  if (m.unit == Unit::None)
    return ClampResolvedBlockLinefeeds(default_lf);
  if (m.negative || (m.unit != Unit::None && m.value == 0))
    return 0;
  const int css_lf = ResolveCssMarginLinefeeds(m, line_h);
  return ClampResolvedBlockLinefeeds(std::max(css_lf, default_lf + 1));
}

inline int ClampHeadingFontSize(int base_px, int px) {
  if (base_px <= 0)
    return px;
  const int min_px = std::max(1, (int)floor((double)base_px * 0.75 + 0.5));
  const int max_px = std::max(min_px, (int)floor((double)base_px * 2.0 + 0.5));
  return std::max(min_px, std::min(px, max_px));
}

inline int DefaultHeadingFontSize(int base_px, int heading_level) {
  double multiplier = 1.0;
  if (heading_level == 1)
    multiplier = 1.5;
  else if (heading_level == 2)
    multiplier = 1.3;
  else if (heading_level == 3)
    multiplier = 1.15;
  return (int)floor((double)base_px * multiplier + 0.5);
}

inline int ComputeHeadingFontSize(
    int base_px, int heading_level, const std::string &style_attr,
    const std::string &class_attr,
    const epub_css_class_map::CssClassMap &class_map) {
  const int fallback_px = DefaultHeadingFontSize(base_px, heading_level);
  book_xml_css_style_utils::FontSizeSpec font_size;
  if (book_xml_css_style_utils::TryParseFontSize(style_attr.c_str(),
                                                 &font_size) ||
      epub_css_class_map::LookupFontSizeForClassAttr(class_attr, class_map,
                                                     &font_size)) {
    const int css_px =
        book_xml_css_style_utils::ResolveFontSizePx(font_size, base_px);
    return ClampHeadingFontSize(base_px, css_px);
  }
  return ClampHeadingFontSize(base_px, fallback_px);
}

} // namespace book_xml_parser_style_utils
