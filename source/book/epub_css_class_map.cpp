#include "book/epub_css_class_map.h"

#include "book/book_xml_css_style_utils.h"
#include "book/epub_css_tokenizer.h"
#include "shared/string_utils.h"

#include <string.h>
#include <vector>

namespace epub_css_class_map {

namespace {

template <typename Fn>
static bool ForEachClass(const std::string &class_attr,
                         const CssClassMap &class_map, Fn fn) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && epub_css_tokenizer::IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && fn(it->second))
      return true;
  }
  return false;
}

} // namespace

void ParseCssIntoClassMap(const char *css_text, size_t len, CssClassMap *out) {
  if (!css_text || len == 0 || !out)
    return;

  size_t pos = 0;
  while (pos < len) {
    epub_css_tokenizer::SkipWhitespace(css_text, len, &pos);
    if (pos >= len)
      break;

    if (pos + 1 < len && css_text[pos] == '/' && css_text[pos + 1] == '*') {
      epub_css_tokenizer::SkipBlockComment(css_text, len, &pos);
      continue;
    }

    size_t selector_start = pos;
    epub_css_tokenizer::SkipToChar(css_text, len, &pos, '{');
    if (pos >= len || css_text[pos] != '{')
      break;
    std::string selector_list(css_text + selector_start, pos - selector_start);
    std::vector<std::string> class_names;
    std::vector<std::string> element_names;
    epub_css_tokenizer::ParseSelectorList(selector_list, &class_names, &element_names);
    ++pos;

    size_t block_start = pos;
    epub_css_tokenizer::SkipToChar(css_text, len, &pos, '}');
    size_t block_end = pos;
    if (pos < len)
      ++pos;

    if (class_names.empty() && element_names.empty())
      continue;

    std::string block(css_text + block_start, block_end - block_start);
    const char *b = block.c_str();

    using book_xml_css_style_utils::ParseMarginBottom;
    using book_xml_css_style_utils::ParseMarginLeft;
    using book_xml_css_style_utils::ParseMarginRight;
    using book_xml_css_style_utils::ParseMarginTop;
    using book_xml_css_style_utils::TextAlign;
    using book_xml_css_style_utils::TryParseTextAlign;

    MarginTopResult mt = ParseMarginTop(b);
    MarginTopResult mb = ParseMarginBottom(b);
    MarginTopResult ml = ParseMarginLeft(b);
    MarginTopResult mr = ParseMarginRight(b);
    TextAlign text_align = TextAlign::Left;
    const bool has_text_align = TryParseTextAlign(b, &text_align);
    WhiteSpaceMode white_space = WhiteSpaceMode::Normal;
    const bool has_white_space =
        book_xml_css_style_utils::TryParseWhiteSpace(b, &white_space);
    FloatMode float_mode = FloatMode::None;
    const bool has_float =
        book_xml_css_style_utils::TryParseFloat(b, &float_mode);
    ClearMode clear_mode = ClearMode::None;
    const bool has_clear =
        book_xml_css_style_utils::TryParseClear(b, &clear_mode);
    FontSizeSpec font_size;
    const bool has_font_size =
        book_xml_css_style_utils::TryParseFontSize(b, &font_size);
    const bool hide_list_markers =
        ContainsNoCase(block, "list-style-type:none") ||
        ContainsNoCase(block, "list-style-type: none") ||
        ContainsNoCase(block, "list-style:none") ||
        ContainsNoCase(block, "list-style: none");
    const bool is_superscript =
        ContainsNoCase(block, "vertical-align:super") ||
        ContainsNoCase(block, "vertical-align: super");
    const bool is_subscript =
        ContainsNoCase(block, "vertical-align:sub") ||
        ContainsNoCase(block, "vertical-align: sub");
    const bool has_page_break_before =
        book_xml_css_style_utils::HasPageBreakBefore(b);
    const bool has_page_break_after =
        book_xml_css_style_utils::HasPageBreakAfter(b);
    const bool has_page_break_inside_avoid =
        book_xml_css_style_utils::HasPageBreakInsideAvoid(b);
    book_xml_css_style_utils::InlineStyleFlags isf{};
    book_xml_css_style_utils::ParseInlineStyleFlags(b, &isf);
    const bool no_underline = isf.no_underline;
    const bool reset_bold   = isf.reset_bold;
    const bool reset_italic = isf.reset_italic;
    const MarginTopResult text_indent =
        book_xml_css_style_utils::ParseTextIndent(b);
    const TextTransform text_transform =
        book_xml_css_style_utils::ParseTextTransform(b);
    const bool has_text_transform = (text_transform != TextTransform::None);
    const bool is_display_block =
        ContainsNoCase(block, "display:block") ||
        ContainsNoCase(block, "display: block");
    const bool is_display_none =
        ContainsNoCase(block, "display:none") ||
        ContainsNoCase(block, "display: none");

    const bool any_prop =
        mt.unit != MarginTopResult::Unit::None ||
        mb.unit != MarginTopResult::Unit::None ||
        ml.unit != MarginTopResult::Unit::None ||
        mr.unit != MarginTopResult::Unit::None ||
        has_font_size || hide_list_markers || has_text_align ||
        has_white_space || has_float || has_clear ||
        is_superscript || is_subscript ||
        has_page_break_before || has_page_break_after ||
        has_page_break_inside_avoid ||
        no_underline || reset_bold || reset_italic ||
        text_indent.unit != MarginTopResult::Unit::None ||
        has_text_transform || is_display_block || is_display_none;

    if (any_prop) {
      // Merge parsed properties into a map entry.
      auto apply = [&](CssClassMargins &entry) {
        if (mt.unit != MarginTopResult::Unit::None)
          entry.margin_top = mt;
        if (mb.unit != MarginTopResult::Unit::None)
          entry.margin_bottom = mb;
        if (ml.unit != MarginTopResult::Unit::None)
          entry.margin_left = ml;
        if (mr.unit != MarginTopResult::Unit::None)
          entry.margin_right = mr;
        if (has_font_size)
          entry.font_size = font_size;
        if (hide_list_markers)
          entry.hide_list_markers = true;
        if (has_text_align) {
          entry.has_text_align = true;
          entry.text_align = text_align;
        }
        if (has_white_space) {
          entry.has_white_space = true;
          entry.white_space = white_space;
        }
        if (has_float) {
          entry.has_float = true;
          entry.float_mode = float_mode;
        }
        if (has_clear) {
          entry.has_clear = true;
          entry.clear_mode = clear_mode;
        }
        if (is_superscript)
          entry.superscript = true;
        if (is_subscript)
          entry.subscript = true;
        if (has_page_break_before)
          entry.page_break_before = true;
        if (has_page_break_after)
          entry.page_break_after = true;
        if (has_page_break_inside_avoid)
          entry.page_break_inside_avoid = true;
        if (no_underline)
          entry.no_underline = true;
        if (reset_bold)
          entry.reset_bold = true;
        if (reset_italic)
          entry.reset_italic = true;
        if (text_indent.unit != MarginTopResult::Unit::None)
          entry.text_indent = text_indent;
        if (has_text_transform) {
          entry.has_text_transform = true;
          entry.text_transform = text_transform;
        }
        if (is_display_block)
          entry.is_display_block = true;
        if (is_display_none)
          entry.is_display_none = true;
      };

      for (size_t i = 0; i < class_names.size(); i++)
        apply((*out)[class_names[i]]);

      // Element selectors: stored under sentinel key "*" + tag_name.
      for (size_t i = 0; i < element_names.size(); i++) {
        std::string key("*");
        key += element_names[i];
        apply((*out)[key]);
      }
    }
  }
}

bool LookupMarginsForClassAttr(const std::string &class_attr,
                               const CssClassMap &class_map,
                               CssClassMargins *out) {
  if (!out)
    return false;
  *out = CssClassMargins{};
  bool found_any = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.margin_top.unit != MarginTopResult::Unit::None) out->margin_top = m.margin_top;
    if (m.margin_bottom.unit != MarginTopResult::Unit::None) out->margin_bottom = m.margin_bottom;
    if (m.margin_left.unit != MarginTopResult::Unit::None) out->margin_left = m.margin_left;
    if (m.margin_right.unit != MarginTopResult::Unit::None) out->margin_right = m.margin_right;
    found_any = true;
    return false;
  });
  return found_any;
}

bool LookupHideListMarkersForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.hide_list_markers; });
}

bool LookupTextAlignForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map,
                                 TextAlign *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.has_text_align) { *out = m.text_align; found = true; }
    return false;
  });
  return found;
}

bool LookupFontSizeForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                FontSizeSpec *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.font_size.unit != FontSizeSpec::Unit::None) { *out = m.font_size; found = true; }
    return false;
  });
  return found;
}

bool LookupSuperSubForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                bool *superscript_out, bool *subscript_out) {
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (superscript_out && m.superscript) { *superscript_out = true; found = true; }
    if (subscript_out && m.subscript) { *subscript_out = true; found = true; }
    return false;
  });
  return found;
}

bool LookupPageBreakBeforeForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.page_break_before; });
}

bool LookupPageBreakAfterForClassAttr(const std::string &class_attr,
                                      const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.page_break_after; });
}

bool LookupPageBreakInsideAvoidForClassAttr(const std::string &class_attr,
                                            const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.page_break_inside_avoid; });
}

bool LookupFloatForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             FloatMode *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.has_float) { *out = m.float_mode; found = true; }
    return false;
  });
  return found;
}

bool LookupClearForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             ClearMode *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.has_clear) { *out = m.clear_mode; found = true; }
    return false;
  });
  return found;
}

bool LookupNoUnderlineForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.no_underline; });
}

bool LookupWhiteSpaceForClassAttr(const std::string &class_attr,
                                  const CssClassMap &class_map,
                                  WhiteSpaceMode *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.has_white_space) { *out = m.white_space; found = true; }
    return false;
  });
  return found;
}

bool LookupResetBoldForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.reset_bold; });
}

MarginTopResult LookupTextIndentForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map) {
  MarginTopResult result{};
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.text_indent.unit != MarginTopResult::Unit::None) {
      result = m.text_indent;
      return true;
    }
    return false;
  });
  return result;
}

bool LookupTextTransformForClassAttr(const std::string &class_attr,
                                     const CssClassMap &class_map,
                                     TextTransform *out) {
  if (!out)
    return false;
  bool found = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.has_text_transform) { *out = m.text_transform; found = true; }
    return false;
  });
  return found;
}

bool LookupResetItalicForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map) {
  return ForEachClass(class_attr, class_map,
    [](const CssClassMargins &m) { return m.reset_italic; });
}

MarginTopResult LookupMarginLeftForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map) {
  MarginTopResult result{};
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.margin_left.unit != MarginTopResult::Unit::None) {
      result = m.margin_left;
      return true;
    }
    return false;
  });
  return result;
}

MarginTopResult LookupMarginRightForClassAttr(const std::string &class_attr,
                                              const CssClassMap &class_map) {
  MarginTopResult result{};
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &m) {
    if (m.margin_right.unit != MarginTopResult::Unit::None) {
      result = m.margin_right;
      return true;
    }
    return false;
  });
  return result;
}

bool LookupAllForClassAttr(const std::string &class_attr,
                           const CssClassMap &class_map,
                           CssClassMargins *out,
                           bool reset_output) {
  if (!out)
    return false;
  if (reset_output)
    *out = CssClassMargins{};
  if (class_attr.empty() || class_map.empty())
    return false;

  bool found_any = false;
  ForEachClass(class_attr, class_map, [&](const CssClassMargins &src) {
    if (src.margin_top.unit != MarginTopResult::Unit::None) out->margin_top = src.margin_top;
    if (src.margin_bottom.unit != MarginTopResult::Unit::None) out->margin_bottom = src.margin_bottom;
    if (src.margin_left.unit != MarginTopResult::Unit::None) out->margin_left = src.margin_left;
    if (src.margin_right.unit != MarginTopResult::Unit::None) out->margin_right = src.margin_right;
    if (src.font_size.unit != FontSizeSpec::Unit::None) out->font_size = src.font_size;
    if (src.hide_list_markers) out->hide_list_markers = true;
    if (src.has_text_align) { out->has_text_align = true; out->text_align = src.text_align; }
    if (src.has_white_space) { out->has_white_space = true; out->white_space = src.white_space; }
    if (src.superscript) out->superscript = true;
    if (src.subscript) out->subscript = true;
    if (src.page_break_before) out->page_break_before = true;
    if (src.page_break_after) out->page_break_after = true;
    if (src.page_break_inside_avoid) out->page_break_inside_avoid = true;
    if (src.has_float) { out->has_float = true; out->float_mode = src.float_mode; }
    if (src.has_clear) { out->has_clear = true; out->clear_mode = src.clear_mode; }
    if (src.no_underline) out->no_underline = true;
    if (src.reset_bold) out->reset_bold = true;
    if (src.reset_italic) out->reset_italic = true;
    if (src.text_indent.unit != MarginTopResult::Unit::None) out->text_indent = src.text_indent;
    if (src.has_text_transform) { out->has_text_transform = true; out->text_transform = src.text_transform; }
    if (src.is_display_block) out->is_display_block = true;
    if (src.is_display_none) out->is_display_none = true;
    found_any = true;
    return false;
  });
  return found_any;
}

bool LookupAllForTag(const char *tag, const CssClassMap &class_map,
                     CssClassMargins *out) {
  if (!out)
    return false;
  *out = CssClassMargins{};
  if (!tag || !tag[0] || class_map.empty())
    return false;
  const size_t tag_len = strlen(tag);
  if (tag_len >= 63)
    return false;
  char key_buf[64];
  key_buf[0] = '*';
  memcpy(key_buf + 1, tag, tag_len + 1);
  CssClassMap::const_iterator it =
      class_map.find(std::string(key_buf, tag_len + 1));
  if (it == class_map.end())
    return false;
  *out = it->second;
  return true;
}

bool MergeClassRulesToStyle(const std::string &class_attr,
                             const CssClassMap &class_map, CssClassMargins *out) {
  return LookupAllForClassAttr(class_attr, class_map, out, false);
}

bool LookupTextAlignForTag(const char *tag, const CssClassMap &class_map,
                            TextAlign *out) {
  if (!tag || !tag[0] || !out || class_map.empty())
    return false;
  const size_t tag_len = strlen(tag);
  if (tag_len >= 63)
    return false;
  char key_buf[64];
  key_buf[0] = '*';
  memcpy(key_buf + 1, tag, tag_len + 1);
  CssClassMap::const_iterator it =
      class_map.find(std::string(key_buf, tag_len + 1));
  if (it == class_map.end() || !it->second.has_text_align)
    return false;
  *out = it->second.text_align;
  return true;
}

bool LookupHideListMarkersForTag(const char *tag,
                                  const CssClassMap &class_map) {
  if (!tag || !tag[0] || class_map.empty())
    return false;
  const size_t tag_len = strlen(tag);
  if (tag_len >= 63)
    return false;
  char key_buf[64];
  key_buf[0] = '*';
  memcpy(key_buf + 1, tag, tag_len + 1);
  CssClassMap::const_iterator it =
      class_map.find(std::string(key_buf, tag_len + 1));
  return it != class_map.end() && it->second.hide_list_markers;
}

} // namespace epub_css_class_map
