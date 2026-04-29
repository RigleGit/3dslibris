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
  // Consume optional fractional part; track whether it is non-zero.
  int frac_x10 = 0;
  bool has_decimal = false;
  if (p < lc.size() && lc[p] == '.') {
    p++;
    has_decimal = true;
    if (p < lc.size() && lc[p] >= '0' && lc[p] <= '9') {
      frac_x10 = lc[p] - '0';
      p++;
      while (p < lc.size() && lc[p] >= '0' && lc[p] <= '9')
        p++;
    }
  }
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
  } else if ((p + 1 < lc.size() && lc[p] == 'e' && lc[p + 1] == 'm') ||
             (p + 2 < lc.size() && lc[p] == 'r' && lc[p + 1] == 'e' &&
              lc[p + 2] == 'm')) {
    // Convert em/rem to approximate pixels (base: 12px).
    const int unit_len = (lc[p] == 'r') ? 3 : 2;
    result.value = value * 12 + (frac_x10 * 12 + 9) / 10;
    result.unit = MarginTopResult::Unit::Px;
    result.negative = negative;
    p += unit_len;
  } else if (value == 0 && !has_decimal) {
    // CSS allows bare unitless zero for lengths (e.g. "margin: 0").
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

bool ParseNumberX100(const std::string &lc, size_t *pos, int *out_value_x100) {
  if (!pos || !out_value_x100)
    return false;

  size_t p = *pos;
  while (p < lc.size() && lc[p] == ' ')
    p++;

  int whole = 0;
  bool have_whole = false;
  while (p < lc.size() && lc[p] >= '0' && lc[p] <= '9') {
    whole = whole * 10 + (lc[p] - '0');
    have_whole = true;
    p++;
  }

  int frac = 0;
  int frac_digits = 0;
  if (p < lc.size() && lc[p] == '.') {
    p++;
    while (p < lc.size() && lc[p] >= '0' && lc[p] <= '9') {
      if (frac_digits < 2) {
        frac = frac * 10 + (lc[p] - '0');
        frac_digits++;
      }
      p++;
    }
  }

  if (!have_whole && frac_digits == 0)
    return false;

  if (frac_digits == 0)
    frac = 0;
  else if (frac_digits == 1)
    frac *= 10;

  *out_value_x100 = whole * 100 + frac;
  *pos = p;
  return true;
}

int RoundDivInt(int num, int den) {
  if (den <= 0)
    return 0;
  if (num >= 0)
    return (num + den / 2) / den;
  return (num - den / 2) / den;
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

bool TryParseFontSize(const char *style, FontSizeSpec *out) {
  if (!out || !style || !style[0])
    return false;

  const std::string lc = ToLowerAscii(std::string(style));
  const std::string prop = "font-size:";
  size_t pos = lc.find(prop);
  if (pos == std::string::npos)
    return false;

  pos += prop.size();
  while (pos < lc.size() && lc[pos] == ' ')
    pos++;

  if (lc.compare(pos, 7, "smaller") == 0) {
    out->unit = FontSizeSpec::Unit::Smaller;
    out->value_x100 = 0;
    return true;
  }
  if (lc.compare(pos, 6, "larger") == 0) {
    out->unit = FontSizeSpec::Unit::Larger;
    out->value_x100 = 0;
    return true;
  }

  int value_x100 = 0;
  if (!ParseNumberX100(lc, &pos, &value_x100))
    return false;

  while (pos < lc.size() && lc[pos] == ' ')
    pos++;

  if (pos + 2 < lc.size() && lc[pos] == 'r' && lc[pos + 1] == 'e' &&
      lc[pos + 2] == 'm') {
    out->unit = FontSizeSpec::Unit::Rem;
  } else if (pos + 1 < lc.size() && lc[pos] == 'e' && lc[pos + 1] == 'm') {
    out->unit = FontSizeSpec::Unit::Em;
  } else if (pos + 1 < lc.size() && lc[pos] == 'p' && lc[pos + 1] == 'x') {
    out->unit = FontSizeSpec::Unit::Px;
  } else if (pos < lc.size() && lc[pos] == '%') {
    out->unit = FontSizeSpec::Unit::Percent;
  } else {
    return false;
  }

  out->value_x100 = value_x100;
  return true;
}

int ResolveFontSizePx(const FontSizeSpec &spec, int base_px) {
  switch (spec.unit) {
  case FontSizeSpec::Unit::Px:
    return RoundDivInt(spec.value_x100, 100);
  case FontSizeSpec::Unit::Percent:
    return RoundDivInt(base_px * spec.value_x100, 10000);
  case FontSizeSpec::Unit::Em:
  case FontSizeSpec::Unit::Rem:
    return RoundDivInt(base_px * spec.value_x100, 100);
  case FontSizeSpec::Unit::Smaller:
    return RoundDivInt(base_px * 100, 120);
  case FontSizeSpec::Unit::Larger:
    return RoundDivInt(base_px * 120, 100);
  case FontSizeSpec::Unit::None:
  default:
    return base_px;
  }
}

bool TryParseTextAlign(const char *style, TextAlign *out) {
  if (!out || !style || !style[0])
    return false;
  const std::string style_lc = ToLowerAscii(std::string(style));
  if (style_lc.find("text-align: center") != std::string::npos ||
      style_lc.find("text-align:center") != std::string::npos) {
    *out = TextAlign::Center;
    return true;
  }
  if (style_lc.find("text-align: right") != std::string::npos ||
      style_lc.find("text-align:right") != std::string::npos) {
    *out = TextAlign::Right;
    return true;
  }
  if (style_lc.find("text-align: justify") != std::string::npos ||
      style_lc.find("text-align:justify") != std::string::npos) {
    *out = TextAlign::Justify;
    return true;
  }
  if (style_lc.find("text-align: left") != std::string::npos ||
      style_lc.find("text-align:left") != std::string::npos) {
    *out = TextAlign::Left;
    return true;
  }
  return false;
}

TextAlign ParseTextAlign(const char *style) {
  TextAlign out = TextAlign::Left;
  if (TryParseTextAlign(style, &out))
    return out;
  return TextAlign::Left;
}

namespace {

// Returns true if the CSS property `prop` (e.g. "page-break-before") appears
// in `lc` with an "always/page/left/right" value. When require_not_dash_preceded
// is true, the match is skipped if the character before the property name is '-'
// (so "break-before" does not match inside "page-break-before").
bool CheckCssBreakProperty(const std::string &lc, const char *prop,
                           bool require_not_dash_preceded) {
  const size_t prop_len = strlen(prop);
  size_t pos = 0;
  while (true) {
    pos = lc.find(prop, pos);
    if (pos == std::string::npos)
      return false;
    if (!require_not_dash_preceded || pos == 0 || lc[pos - 1] != '-') {
      size_t colon = lc.find(':', pos + prop_len);
      if (colon != std::string::npos) {
        size_t v = colon + 1;
        while (v < lc.size() && lc[v] == ' ')
          v++;
        if (lc.compare(v, 6, "always") == 0)
          return true;
        if (lc.compare(v, 4, "page") == 0)
          return true;
        if (lc.compare(v, 5, "right") == 0)
          return true;
        if (lc.compare(v, 4, "left") == 0)
          return true;
      }
    }
    pos++;
  }
}

} // anonymous namespace (page-break helpers)

bool HasPageBreakBefore(const char *style) {
  if (!style || !style[0])
    return false;
  const std::string lc = ToLowerAscii(std::string(style));
  return CheckCssBreakProperty(lc, "page-break-before", false) ||
         CheckCssBreakProperty(lc, "break-before", true);
}

bool HasPageBreakAfter(const char *style) {
  if (!style || !style[0])
    return false;
  const std::string lc = ToLowerAscii(std::string(style));
  return CheckCssBreakProperty(lc, "page-break-after", false) ||
         CheckCssBreakProperty(lc, "break-after", true);
}

} // namespace book_xml_css_style_utils
