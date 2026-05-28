#pragma once

#include "shared/text_token_constants.h"

#include <cstddef>
#include <string>

namespace book_xml_css_style_utils {

struct MarginTopResult {
  enum class Unit { None, Px, Percent, Em };
  int value;  // for Em: hundredths of em (1.5em → 150)
  Unit unit;
  bool negative;

  MarginTopResult() : value(0), unit(Unit::None), negative(false) {}
};

MarginTopResult ParseMarginTop(const char *style);
MarginTopResult ParseMarginBottom(const char *style);
MarginTopResult ParseMarginLeft(const char *style);
MarginTopResult ParseMarginRight(const char *style);
MarginTopResult ParsePaddingTop(const char *style);
int ResolveHorizontalMarginPx(const MarginTopResult &mtr, int display_width,
                              int font_size_px = 0);

struct FontSizeSpec {
  enum class Unit { None, Px, Percent, Em, Rem, Smaller, Larger };
  int value_x100;
  Unit unit;
  bool is_keyword;

  FontSizeSpec() : value_x100(0), unit(Unit::None), is_keyword(false) {}
};

bool TryParseFontSize(const char *style, FontSizeSpec *out);
int ResolveFontSizePx(const FontSizeSpec &spec, int base_px,
                      int css_baseline_px = 16);

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

MarginTopResult ParseWidth(const char *style);

enum class WhiteSpaceMode { Normal, Nowrap, Pre, PreWrap, PreLine };

WhiteSpaceMode ParseWhiteSpace(const char *style);
bool TryParseWhiteSpace(const char *style, WhiteSpaceMode *out);
std::string NormalizeWhiteSpaceText(const char *utf8, size_t len,
                                    WhiteSpaceMode mode);

enum class FloatMode { None, Left, Right };
FloatMode ParseFloat(const char *style);
bool TryParseFloat(const char *style, FloatMode *out);

enum class ClearMode { None, Left, Right, Both };
ClearMode ParseClear(const char *style);
bool TryParseClear(const char *style, ClearMode *out);

bool HasPageBreakBefore(const char *style);
bool HasPageBreakAfter(const char *style);
bool HasPageBreakInsideAvoid(const char *style);

// Returns the text-indent length, or Unit::None if not specified.
MarginTopResult ParseTextIndent(const char *style);

enum class TextTransform { None, Uppercase, Lowercase, Capitalize };
TextTransform ParseTextTransform(const char *style);

} // namespace book_xml_css_style_utils
