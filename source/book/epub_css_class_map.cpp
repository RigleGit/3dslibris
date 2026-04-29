#include "book/epub_css_class_map.h"

#include "book/book_xml_css_style_utils.h"
#include "shared/string_utils.h"

#include <string.h>
#include <vector>

namespace epub_css_class_map {

namespace {

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

std::string TrimAscii(const std::string &text) {
  size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t' ||
                                 text[start] == '\r' || text[start] == '\n'))
    ++start;
  size_t end = text.size();
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n'))
    --end;
  return text.substr(start, end - start);
}

bool ContainsNoCase(const std::string &haystack, const char *needle) {
  if (!needle || !needle[0])
    return false;
  const std::string lower_haystack = ToLowerAscii(haystack.c_str());
  const std::string lower_needle = ToLowerAscii(needle);
  return lower_haystack.find(lower_needle) != std::string::npos;
}

void SkipWhitespace(const char *s, size_t len, size_t *pos) {
  while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' ||
                        s[*pos] == '\r' || s[*pos] == '\n'))
    ++(*pos);
}

void SkipToChar(const char *s, size_t len, size_t *pos, char target) {
  while (*pos < len && s[*pos] != target)
    ++(*pos);
}

void SkipBlockComment(const char *s, size_t len, size_t *pos) {
  if (*pos + 1 < len && s[*pos] == '/' && s[*pos + 1] == '*') {
    *pos += 2;
    while (*pos + 1 < len) {
      if (s[*pos] == '*' && s[*pos + 1] == '/') {
        *pos += 2;
        return;
      }
      ++(*pos);
    }
    *pos = len;
  }
}

bool ExtractSingleClassSelectorName(const std::string &selector,
                                    std::string *class_name_out) {
  if (!class_name_out)
    return false;

  const std::string trimmed = TrimAscii(selector);
  if (trimmed.empty())
    return false;

  // Support ".class" and "tag.class". Reject combinators, descendant
  // selectors, pseudo classes, and compound selectors.
  size_t pos = 0;
  while (pos < trimmed.size() && IsIdentChar(trimmed[pos]))
    ++pos;
  if (pos >= trimmed.size() || trimmed[pos] != '.')
    return false;
  ++pos;

  const size_t class_start = pos;
  while (pos < trimmed.size() && IsIdentChar(trimmed[pos]))
    ++pos;
  if (pos == class_start || pos != trimmed.size())
    return false;

  *class_name_out = trimmed.substr(class_start, pos - class_start);
  return true;
}

void ParseSelectorList(const std::string &selector_list,
                       std::vector<std::string> *class_names_out) {
  if (!class_names_out)
    return;
  size_t start = 0;
  while (start <= selector_list.size()) {
    size_t comma = selector_list.find(',', start);
    std::string selector =
        selector_list.substr(start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start);
    std::string class_name;
    if (ExtractSingleClassSelectorName(selector, &class_name))
      class_names_out->push_back(class_name);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
}

} // namespace

void ParseCssIntoClassMap(const char *css_text, size_t len, CssClassMap *out) {
  if (!css_text || len == 0 || !out)
    return;

  size_t pos = 0;
  while (pos < len) {
    SkipWhitespace(css_text, len, &pos);
    if (pos >= len)
      break;

    if (pos + 1 < len && css_text[pos] == '/' && css_text[pos + 1] == '*') {
      SkipBlockComment(css_text, len, &pos);
      continue;
    }

    size_t selector_start = pos;
    SkipToChar(css_text, len, &pos, '{');
    if (pos >= len || css_text[pos] != '{')
      break;
    std::string selector_list(css_text + selector_start, pos - selector_start);
    std::vector<std::string> class_names;
    ParseSelectorList(selector_list, &class_names);
    ++pos;

    size_t block_start = pos;
    SkipToChar(css_text, len, &pos, '}');
    size_t block_end = pos;
    if (pos < len)
      ++pos;

    if (class_names.empty())
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

    if (mt.unit != MarginTopResult::Unit::None ||
        mb.unit != MarginTopResult::Unit::None ||
        ml.unit != MarginTopResult::Unit::None ||
        mr.unit != MarginTopResult::Unit::None ||
        has_font_size ||
        hide_list_markers ||
        has_text_align || has_white_space ||
        has_float || has_clear ||
        is_superscript || is_subscript ||
        has_page_break_before || has_page_break_after ||
        has_page_break_inside_avoid ||
        no_underline || reset_bold || reset_italic ||
        text_indent.unit != MarginTopResult::Unit::None ||
        has_text_transform) {
      for (size_t i = 0; i < class_names.size(); i++) {
        CssClassMargins &entry = (*out)[class_names[i]];
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
  if (class_attr.empty() || class_map.empty())
    return false;

  bool found_any = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }

    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it == class_map.end())
      continue;
    if (it->second.margin_top.unit != MarginTopResult::Unit::None)
      out->margin_top = it->second.margin_top;
    if (it->second.margin_bottom.unit != MarginTopResult::Unit::None)
      out->margin_bottom = it->second.margin_bottom;
    if (it->second.margin_left.unit != MarginTopResult::Unit::None)
      out->margin_left = it->second.margin_left;
    if (it->second.margin_right.unit != MarginTopResult::Unit::None)
      out->margin_right = it->second.margin_right;
    found_any = true;
  }

  return found_any;
}

bool LookupHideListMarkersForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;

  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }

    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.hide_list_markers)
      return true;
  }

  return false;
}

bool LookupTextAlignForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map,
                                 TextAlign *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;

  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }

    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it == class_map.end() || !it->second.has_text_align)
      continue;
    *out = it->second.text_align;
    found = true;
  }
  return found;
}

bool LookupFontSizeForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                FontSizeSpec *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;

  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }

    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it == class_map.end() ||
        it->second.font_size.unit == FontSizeSpec::Unit::None)
      continue;
    *out = it->second.font_size;
    found = true;
  }

  return found;
}

bool LookupSuperSubForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                bool *superscript_out, bool *subscript_out) {
  if (class_attr.empty() || class_map.empty())
    return false;

  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }

    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it == class_map.end())
      continue;
    if (superscript_out && it->second.superscript) {
      *superscript_out = true;
      found = true;
    }
    if (subscript_out && it->second.subscript) {
      *subscript_out = true;
      found = true;
    }
  }
  return found;
}

bool LookupPageBreakBeforeForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.page_break_before)
      return true;
  }
  return false;
}

bool LookupPageBreakAfterForClassAttr(const std::string &class_attr,
                                      const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.page_break_after)
      return true;
  }
  return false;
}

bool LookupPageBreakInsideAvoidForClassAttr(const std::string &class_attr,
                                            const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.page_break_inside_avoid)
      return true;
  }
  return false;
}

bool LookupFloatForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             FloatMode *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;
  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.has_float) {
      *out = it->second.float_mode;
      found = true;
    }
  }
  return found;
}

bool LookupClearForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             ClearMode *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;
  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.has_clear) {
      *out = it->second.clear_mode;
      found = true;
    }
  }
  return found;
}

bool LookupNoUnderlineForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.no_underline)
      return true;
  }
  return false;
}

bool LookupWhiteSpaceForClassAttr(const std::string &class_attr,
                                  const CssClassMap &class_map,
                                  WhiteSpaceMode *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  bool found = false;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) {
      if (pos < class_attr.size())
        ++pos;
      continue;
    }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.has_white_space) {
      *out = it->second.white_space;
      found = true;
    }
  }
  return found;
}

bool LookupResetBoldForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.reset_bold)
      return true;
  }
  return false;
}

MarginTopResult LookupTextIndentForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return MarginTopResult{};
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() &&
        it->second.text_indent.unit != MarginTopResult::Unit::None)
      return it->second.text_indent;
  }
  return MarginTopResult{};
}

bool LookupTextTransformForClassAttr(const std::string &class_attr,
                                     const CssClassMap &class_map,
                                     TextTransform *out) {
  if (!out || class_attr.empty() || class_map.empty())
    return false;
  bool found = false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.has_text_transform) {
      *out = it->second.text_transform;
      found = true;
    }
  }
  return found;
}

bool LookupResetItalicForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return false;
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() && it->second.reset_italic)
      return true;
  }
  return false;
}

MarginTopResult LookupMarginLeftForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return MarginTopResult{};
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() &&
        it->second.margin_left.unit != MarginTopResult::Unit::None)
      return it->second.margin_left;
  }
  return MarginTopResult{};
}

MarginTopResult LookupMarginRightForClassAttr(const std::string &class_attr,
                                              const CssClassMap &class_map) {
  if (class_attr.empty() || class_map.empty())
    return MarginTopResult{};
  size_t pos = 0;
  while (pos < class_attr.size()) {
    while (pos < class_attr.size() &&
           (class_attr[pos] == ' ' || class_attr[pos] == '\t' ||
            class_attr[pos] == '\r' || class_attr[pos] == '\n'))
      ++pos;
    const size_t start = pos;
    while (pos < class_attr.size() && IsIdentChar(class_attr[pos]))
      ++pos;
    if (pos == start) { if (pos < class_attr.size()) ++pos; continue; }
    const std::string class_name = class_attr.substr(start, pos - start);
    CssClassMap::const_iterator it = class_map.find(class_name);
    if (it != class_map.end() &&
        it->second.margin_right.unit != MarginTopResult::Unit::None)
      return it->second.margin_right;
  }
  return MarginTopResult{};
}

} // namespace epub_css_class_map
