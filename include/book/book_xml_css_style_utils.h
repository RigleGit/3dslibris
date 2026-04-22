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

struct InlineStyleFlags {
  bool bold;
  bool italic;
  bool underline;
  unsigned char underline_style;
  bool overline;
  bool strikethrough;
  bool superscript;
  bool subscript;

  InlineStyleFlags()
      : bold(false), italic(false), underline(false),
        underline_style(UNDERLINE_STYLE_SOLID), overline(false),
        strikethrough(false), superscript(false), subscript(false) {}
};

void ParseInlineStyleFlags(const char *style, InlineStyleFlags *out);

enum class TextAlign { Left, Center, Right, Justify };

TextAlign ParseTextAlign(const char *style);

} // namespace book_xml_css_style_utils
