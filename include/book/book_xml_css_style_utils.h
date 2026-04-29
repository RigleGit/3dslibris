#pragma once

#include "shared/text_token_constants.h"

namespace book_xml_css_style_utils {

struct MarginTopResult {
  enum class Unit { None, Px, Percent };
  int value;
  Unit unit;
  bool negative;

  MarginTopResult() : value(0), unit(Unit::None), negative(false) {}
};

MarginTopResult ParseMarginTop(const char *style);
MarginTopResult ParseMarginBottom(const char *style);

struct FontSizeSpec {
  enum class Unit { None, Px, Percent, Em, Rem, Smaller, Larger };
  int value_x100;
  Unit unit;

  FontSizeSpec() : value_x100(0), unit(Unit::None) {}
};

bool TryParseFontSize(const char *style, FontSizeSpec *out);
int ResolveFontSizePx(const FontSizeSpec &spec, int base_px);

struct InlineStyleFlags {
  bool bold;
  bool italic;
  bool underline;
  unsigned char underline_style;
  bool overline;
  bool strikethrough;
  bool superscript;
  bool subscript;
  bool no_underline;  // text-decoration: none
  bool reset_bold;    // font-weight: normal/lighter/100-500
  bool reset_italic;  // font-style: normal

  InlineStyleFlags()
      : bold(false), italic(false), underline(false),
        underline_style(UNDERLINE_STYLE_SOLID), overline(false),
        strikethrough(false), superscript(false), subscript(false),
        no_underline(false), reset_bold(false), reset_italic(false) {}
};

void ParseInlineStyleFlags(const char *style, InlineStyleFlags *out);

enum class TextAlign { Left, Center, Right, Justify };

TextAlign ParseTextAlign(const char *style);
bool TryParseTextAlign(const char *style, TextAlign *out);

bool HasPageBreakBefore(const char *style);
bool HasPageBreakAfter(const char *style);

} // namespace book_xml_css_style_utils
