// Inline text formatting CSS properties: bold, italic, underline, font-size,
// text-transform. Block layout properties are in book_xml_css_style_utils.cpp.

#include "book/book_xml_css_style_utils.h"

#include "shared/string_utils.h"

#include <string>

namespace book_xml_css_style_utils {

namespace {

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
  if (style_lc.find("double") != std::string::npos)
    return UNDERLINE_STYLE_DOUBLE;
  return UNDERLINE_STYLE_SOLID;
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

  // text-decoration: none (find property then check value is "none")
  {
    size_t p2 = 0;
    while (p2 < style_lc.size()) {
      p2 = style_lc.find("text-decoration", p2);
      if (p2 == std::string::npos) break;
      size_t colon = style_lc.find(':', p2);
      if (colon == std::string::npos) break;
      size_t v = colon + 1;
      while (v < style_lc.size() && style_lc[v] == ' ') v++;
      if (style_lc.compare(v, 4, "none") == 0) {
        out->no_underline = true;
        break;
      }
      p2++;
    }
  }

  if (ContainsNoCase(style_lc, "font-weight:normal") ||
      ContainsNoCase(style_lc, "font-weight: normal") ||
      ContainsNoCase(style_lc, "font-weight:lighter") ||
      ContainsNoCase(style_lc, "font-weight: lighter") ||
      ContainsNoCase(style_lc, "font-weight:100") ||
      ContainsNoCase(style_lc, "font-weight: 100") ||
      ContainsNoCase(style_lc, "font-weight:200") ||
      ContainsNoCase(style_lc, "font-weight: 200") ||
      ContainsNoCase(style_lc, "font-weight:300") ||
      ContainsNoCase(style_lc, "font-weight: 300") ||
      ContainsNoCase(style_lc, "font-weight:400") ||
      ContainsNoCase(style_lc, "font-weight: 400") ||
      ContainsNoCase(style_lc, "font-weight:500") ||
      ContainsNoCase(style_lc, "font-weight: 500")) {
    out->reset_bold = true;
  }

  if (ContainsNoCase(style_lc, "font-style:normal") ||
      ContainsNoCase(style_lc, "font-style: normal")) {
    out->reset_italic = true;
  }
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
    out->is_keyword = true;
    return true;
  }
  if (lc.compare(pos, 6, "larger") == 0) {
    out->unit = FontSizeSpec::Unit::Larger;
    out->value_x100 = 0;
    out->is_keyword = true;
    return true;
  }
  // Absolute keyword sizes — stored as Percent relative to base font.
  // Order: longer prefixes first to avoid xx-large matching x-large.
  if (lc.compare(pos, 8, "xx-large") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 20000; out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 7, "x-large") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 15000; out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 5, "large") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 12500; out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 6, "medium") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 10000; out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 8, "xx-small") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 5000;  out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 7, "x-small") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 6250;  out->is_keyword = true; return true;
  }
  if (lc.compare(pos, 5, "small") == 0) {
    out->unit = FontSizeSpec::Unit::Percent; out->value_x100 = 8000;  out->is_keyword = true; return true;
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
  } else if (pos + 1 < lc.size() && lc[pos] == 'p' && lc[pos + 1] == 't') {
    // 1pt = 4/3 px; value_x100 is in units of hundredths of px.
    out->unit = FontSizeSpec::Unit::Px;
    out->value_x100 = (value_x100 * 4 + 1) / 3;
    return true;
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
    // Scale publisher px relative to the standard 16px CSS baseline so that
    // the user's font-size preference acts as a global scale factor.
    return RoundDivInt(spec.value_x100 * base_px, 16 * 100);
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

TextTransform ParseTextTransform(const char *style) {
  if (!style || !style[0])
    return TextTransform::None;
  const std::string lc = ToLowerAscii(std::string(style));
  const std::string key = "text-transform:";
  size_t pos = lc.find(key);
  if (pos == std::string::npos)
    return TextTransform::None;
  pos += key.size();
  while (pos < lc.size() && lc[pos] == ' ')
    pos++;
  if (lc.compare(pos, 9, "uppercase") == 0) return TextTransform::Uppercase;
  if (lc.compare(pos, 9, "lowercase") == 0) return TextTransform::Lowercase;
  if (lc.compare(pos, 10, "capitalize") == 0) return TextTransform::Capitalize;
  return TextTransform::None;
}

} // namespace book_xml_css_style_utils
