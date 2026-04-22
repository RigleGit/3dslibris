#include "book/book_xml_css_style_utils.h"

#include "shared/string_utils.h"

#include <string>

namespace book_xml_css_style_utils {

namespace {

bool ContainsNoCase(const std::string &haystack, const char *needle) {
  return needle && needle[0] &&
         haystack.find(ToLowerAscii(std::string(needle))) != std::string::npos;
}

bool HasTextDecorationKeyword(const std::string &style_lc,
                              const char *keyword) {
  if (!keyword || !keyword[0])
    return false;
  const bool mentions_text_decoration =
      style_lc.find("text-decoration") != std::string::npos ||
      style_lc.find("text-decoration-line") != std::string::npos;
  return mentions_text_decoration &&
         style_lc.find(keyword) != std::string::npos;
}

unsigned char ParseUnderlineStyle(const std::string &style_lc) {
  const bool mentions_text_decoration =
      style_lc.find("text-decoration") != std::string::npos ||
      style_lc.find("text-decoration-line") != std::string::npos ||
      style_lc.find("text-decoration-style") != std::string::npos;
  if (!mentions_text_decoration)
    return UNDERLINE_STYLE_SOLID;
  if (style_lc.find("wavy") != std::string::npos)
    return UNDERLINE_STYLE_WAVY;
  if (style_lc.find("dashed") != std::string::npos)
    return UNDERLINE_STYLE_DASHED;
  if (style_lc.find("dotted") != std::string::npos)
    return UNDERLINE_STYLE_DOTTED;
  return UNDERLINE_STYLE_SOLID;
}

// Parses a single CSS length token (e.g. "5%", "12px", "-3px") from lc[*pos].
// Advances *pos past the consumed token. Returns unit==None on parse failure.
MarginTopResult ParseOneLengthToken(const std::string &lc, size_t *pos) {
  MarginTopResult result;
  size_t p = *pos;
  while (p < lc.size() && lc[p] == ' ')
    p++;
  if (p >= lc.size())
    return result;
  bool negative = false;
  if (lc[p] == '-') {
    negative = true;
    p++;
  }
  int value = 0;
  bool has_digit = false;
  while (p < lc.size() && lc[p] >= '0' && lc[p] <= '9') {
    value = value * 10 + (lc[p] - '0');
    has_digit = true;
    p++;
  }
  if (!has_digit)
    return result;
  while (p < lc.size() && lc[p] == ' ')
    p++;
  if (p < lc.size() && lc[p] == '%') {
    result.value = value;
    result.unit = MarginTopResult::Unit::Percent;
    result.negative = negative;
    p++;
  } else if (p + 1 < lc.size() && lc[p] == 'p' && lc[p + 1] == 'x') {
    result.value = value;
    result.unit = MarginTopResult::Unit::Px;
    result.negative = negative;
    p += 2;
  } else if (value == 0) {
    // CSS allows unitless zero for lengths.
    result.value = 0;
    result.unit = MarginTopResult::Unit::Px;
    result.negative = negative;
  } else {
    return result;
  }
  *pos = p;
  return result;
}

// Parses "margin: <top> [<right> [<bottom> [<left>]]]" starting at start_pos.
// which=0 → top value, which=2 → bottom value.
// CSS shorthand: 1 value → all sides equal; 2 values → top=bottom, right=left;
// 3 values → top, right=left, bottom; 4 values → each side explicit.
MarginTopResult ParseMarginShorthand(const std::string &lc,
                                     size_t start_pos, int which) {
  MarginTopResult tokens[4];
  int count = 0;
  size_t p = start_pos;
  while (count < 4 && p < lc.size() && lc[p] != ';') {
    MarginTopResult t = ParseOneLengthToken(lc, &p);
    if (t.unit == MarginTopResult::Unit::None)
      break;
    tokens[count++] = t;
    while (p < lc.size() && lc[p] == ' ')
      p++;
  }
  if (count == 0)
    return MarginTopResult{};
  if (which == 0)
    return tokens[0];
  if (which == 2) {
    if (count >= 3) return tokens[2];
    return tokens[0]; // 1 or 2 values: bottom equals top
  }
  return tokens[0];
}

} // namespace

void ParseInlineStyleFlags(const char *style, InlineStyleFlags *out) {
  if (!style || !out)
    return;

  const std::string style_lc = ToLowerAscii(std::string(style));

  if (ContainsNoCase(style_lc, "font-style:italic") ||
      ContainsNoCase(style_lc, "font-style: italic") ||
      ContainsNoCase(style_lc, "font-style:oblique") ||
      ContainsNoCase(style_lc, "font-style: oblique") ||
      ContainsNoCase(style_lc, "font:italic") ||
      ContainsNoCase(style_lc, "font: italic") ||
      ContainsNoCase(style_lc, "font:oblique") ||
      ContainsNoCase(style_lc, "font: oblique")) {
    out->italic = true;
  }

  if (ContainsNoCase(style_lc, "font-weight:bold") ||
      ContainsNoCase(style_lc, "font-weight: bold") ||
      ContainsNoCase(style_lc, "font-weight:bolder") ||
      ContainsNoCase(style_lc, "font-weight: bolder") ||
      ContainsNoCase(style_lc, "font-weight:600") ||
      ContainsNoCase(style_lc, "font-weight: 600") ||
      ContainsNoCase(style_lc, "font-weight:700") ||
      ContainsNoCase(style_lc, "font-weight: 700") ||
      ContainsNoCase(style_lc, "font-weight:800") ||
      ContainsNoCase(style_lc, "font-weight: 800") ||
      ContainsNoCase(style_lc, "font-weight:900") ||
      ContainsNoCase(style_lc, "font-weight: 900") ||
      ContainsNoCase(style_lc, "font:bold") ||
      ContainsNoCase(style_lc, "font: bold")) {
    out->bold = true;
  }

  if (HasTextDecorationKeyword(style_lc, "underline")) {
    out->underline = true;
    out->underline_style = ParseUnderlineStyle(style_lc);
  }
  if (HasTextDecorationKeyword(style_lc, "overline"))
    out->overline = true;
  if (HasTextDecorationKeyword(style_lc, "line-through"))
    out->strikethrough = true;

  if (ContainsNoCase(style_lc, "vertical-align:super") ||
      ContainsNoCase(style_lc, "vertical-align: super")) {
    out->superscript = true;
  }

  if (ContainsNoCase(style_lc, "vertical-align:sub") ||
      ContainsNoCase(style_lc, "vertical-align: sub")) {
    out->subscript = true;
  }
}

static MarginTopResult ParseMarginValue(const char *style, const char *property,
                                        int shorthand_which) {
  MarginTopResult result;
  if (!style || !style[0])
    return result;
  const std::string lc = ToLowerAscii(std::string(style));

  const std::string prop_colon = std::string(property) + ":";
  size_t pos = lc.find(prop_colon);
  if (pos != std::string::npos) {
    pos += prop_colon.size();
    while (pos < lc.size() && lc[pos] == ' ')
      pos++;
    if (pos < lc.size() && lc[pos] == '-') {
      result.negative = true;
      pos++;
    }
    int value = 0;
    bool has_digit = false;
    while (pos < lc.size() && lc[pos] >= '0' && lc[pos] <= '9') {
      value = value * 10 + (lc[pos] - '0');
      has_digit = true;
      pos++;
    }
    if (has_digit) {
      while (pos < lc.size() && lc[pos] == ' ')
        pos++;
      if (pos < lc.size() && lc[pos] == '%') {
        result.value = value;
        result.unit = MarginTopResult::Unit::Percent;
        return result;
      }
      if (pos + 1 < lc.size() && lc[pos] == 'p' && lc[pos + 1] == 'x') {
        result.value = value;
        result.unit = MarginTopResult::Unit::Px;
        return result;
      }
      if (value == 0) {
        result.value = 0;
        result.unit = MarginTopResult::Unit::Px;
        return result;
      }
    }
    result = MarginTopResult{};
  }

  const char *shorthand = "margin:";
  pos = lc.find(shorthand);
  if (pos == std::string::npos) {
    shorthand = "margin: ";
    pos = lc.find(shorthand);
  }
  if (pos == std::string::npos)
    return result;
  pos += strlen(shorthand);
  return ParseMarginShorthand(lc, pos, shorthand_which);
}

MarginTopResult ParseMarginTop(const char *style) {
  return ParseMarginValue(style, "margin-top", 0);
}

MarginTopResult ParseMarginBottom(const char *style) {
  return ParseMarginValue(style, "margin-bottom", 2);
}

TextAlign ParseTextAlign(const char *style) {
  if (!style || !style[0])
    return TextAlign::Left;
  const std::string style_lc = ToLowerAscii(std::string(style));
  if (style_lc.find("text-align: center") != std::string::npos ||
      style_lc.find("text-align:center") != std::string::npos) {
    return TextAlign::Center;
  }
  if (style_lc.find("text-align: right") != std::string::npos ||
      style_lc.find("text-align:right") != std::string::npos) {
    return TextAlign::Right;
  }
  if (style_lc.find("text-align: justify") != std::string::npos ||
      style_lc.find("text-align:justify") != std::string::npos) {
    return TextAlign::Justify;
  }
  return TextAlign::Left;
}

} // namespace book_xml_css_style_utils

