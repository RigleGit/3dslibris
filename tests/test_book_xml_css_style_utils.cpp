#include "book/book_xml_css_style_utils.h"

#include "test_assert.h"

namespace {

void TestDetectsOverlineDecoration() {
  book_xml_css_style_utils::InlineStyleFlags flags{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "text-decoration: overline;", &flags);

  test::ExpectTrue("overline detected", flags.overline);
  test::ExpectFalse("underline not implied", flags.underline);
}

void TestDetectsUnderlineRegardlessOfDecorationOrder() {
  book_xml_css_style_utils::InlineStyleFlags flags{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "text-decoration: wavy underline;", &flags);

  test::ExpectTrue("underline detected", flags.underline);
  test::ExpectEq("wavy underline style", (int)flags.underline_style,
                 UNDERLINE_STYLE_WAVY);
  test::ExpectFalse("strikethrough not implied", flags.strikethrough);
}

void TestDetectsDashedAndDottedUnderlineStyles() {
  book_xml_css_style_utils::InlineStyleFlags dashed{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "text-decoration: underline dashed;", &dashed);
  test::ExpectTrue("dashed underline detected", dashed.underline);
  test::ExpectEq("dashed underline style", (int)dashed.underline_style,
                 UNDERLINE_STYLE_DASHED);

  book_xml_css_style_utils::InlineStyleFlags dotted{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "text-decoration-style: dotted; text-decoration-line: underline;",
      &dotted);
  test::ExpectTrue("dotted underline detected", dotted.underline);
  test::ExpectEq("dotted underline style", (int)dotted.underline_style,
                 UNDERLINE_STYLE_DOTTED);
}

void TestDetectsDoubleUnderlineStyle() {
  book_xml_css_style_utils::InlineStyleFlags flags{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "text-decoration: underline double;", &flags);
  test::ExpectTrue("double underline detected", flags.underline);
  test::ExpectEq("double underline style", (int)flags.underline_style,
                 UNDERLINE_STYLE_DOUBLE);
}

void TestDetectsBoldItalicAndVerticalAlign() {
  book_xml_css_style_utils::InlineStyleFlags flags{};
  book_xml_css_style_utils::ParseInlineStyleFlags(
      "font-weight: bold; font-style: italic; vertical-align: super;", &flags);

  test::ExpectTrue("bold detected", flags.bold);
  test::ExpectTrue("italic detected", flags.italic);
  test::ExpectTrue("superscript detected", flags.superscript);
  test::ExpectFalse("subscript not implied", flags.subscript);
}

void TestParseMarginTopPositivePx() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 20px;");
  test::ExpectEq("px unit",  (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("px value", r.value,     20);
  test::ExpectFalse("not negative", r.negative);
}

void TestParseMarginTopNegativePx() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: -10px;");
  test::ExpectEq("px unit",    (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("px value",   r.value,     10);
  test::ExpectTrue("negative flag", r.negative);
}

void TestParseMarginTopPercent() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 5%;");
  test::ExpectEq("percent unit",  (int)r.unit, (int)R::Unit::Percent);
  test::ExpectEq("percent value", r.value,     5);
  test::ExpectFalse("not negative", r.negative);
}

void TestParseMarginTopZeroUnitless() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 0;");
  test::ExpectEq("zero unitless -> Px", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("zero unitless value", r.value, 0);
}

void TestParseMarginTopZeroPx() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 0px;");
  test::ExpectEq("zero px unit",  (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("zero px value", r.value,     0);
}

void TestParseMarginTopMissingProperty() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("font-weight: bold;");
  test::ExpectEq("missing -> None", (int)r.unit, (int)R::Unit::None);
}

void TestParseMarginTopNull() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop(nullptr);
  test::ExpectEq("null -> None", (int)r.unit, (int)R::Unit::None);
}

void TestParseMarginTopEmUnit() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 2em;");
  test::ExpectEq("em -> Px", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("em value (2 * 12px base)", r.value, 24);
}

void TestParseMarginTopPtUnit() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginTop("margin-top: 12pt;");
  test::ExpectEq("pt -> Px", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("12pt -> 16px", r.value, 16);
}

void TestTryParseFontSizePt() {
  book_xml_css_style_utils::FontSizeSpec spec{};
  const bool ok = book_xml_css_style_utils::TryParseFontSize("font-size: 12pt;", &spec);
  test::ExpectTrue("font-size pt parsed", ok);
  test::ExpectEq("pt stored as Px", (int)spec.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Px);
  // 12pt * 4/3 = 16px; value_x100 = 1600
  test::ExpectEq("12pt -> 1600 value_x100", spec.value_x100, 1600);
}

void TestTryParseFontSizeAbsoluteKeywords() {
  using S = book_xml_css_style_utils::FontSizeSpec;
  struct { const char *css; int expected_x100; } cases[] = {
    { "font-size: xx-small;", 5000 },
    { "font-size: x-small;",  6250 },
    { "font-size: small;",    8000 },
    { "font-size: medium;",  10000 },
    { "font-size: large;",   12500 },
    { "font-size: x-large;", 15000 },
    { "font-size: xx-large;",20000 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    S spec{};
    test::ExpectTrue(cases[i].css,
                     book_xml_css_style_utils::TryParseFontSize(cases[i].css, &spec));
    test::ExpectEq((std::string(cases[i].css) + " unit").c_str(),
                   (int)spec.unit, (int)S::Unit::Percent);
    test::ExpectEq((std::string(cases[i].css) + " value").c_str(),
                   spec.value_x100, cases[i].expected_x100);
  }
}

void TestParseInlineFlagsResets() {
  using book_xml_css_style_utils::InlineStyleFlags;
  using book_xml_css_style_utils::ParseInlineStyleFlags;

  InlineStyleFlags none_flags{};
  ParseInlineStyleFlags("text-decoration: none;", &none_flags);
  test::ExpectTrue("text-decoration:none sets no_underline", none_flags.no_underline);
  test::ExpectFalse("text-decoration:none does not set underline", none_flags.underline);

  InlineStyleFlags normal_weight{};
  ParseInlineStyleFlags("font-weight: normal;", &normal_weight);
  test::ExpectTrue("font-weight:normal sets reset_bold", normal_weight.reset_bold);
  test::ExpectFalse("font-weight:normal does not set bold", normal_weight.bold);

  InlineStyleFlags w400{};
  ParseInlineStyleFlags("font-weight: 400;", &w400);
  test::ExpectTrue("font-weight:400 sets reset_bold", w400.reset_bold);

  InlineStyleFlags normal_style{};
  ParseInlineStyleFlags("font-style: normal;", &normal_style);
  test::ExpectTrue("font-style:normal sets reset_italic", normal_style.reset_italic);
  test::ExpectFalse("font-style:normal does not set italic", normal_style.italic);

  InlineStyleFlags bold_flags{};
  ParseInlineStyleFlags("font-weight: bold;", &bold_flags);
  test::ExpectTrue("font-weight:bold sets bold", bold_flags.bold);
  test::ExpectFalse("font-weight:bold does not reset_bold", bold_flags.reset_bold);
}

void TestParseMarginBottomZeroUnitless() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginBottom("margin-bottom: 0;");
  test::ExpectEq("bottom zero unitless -> Px", (int)r.unit,
                 (int)R::Unit::Px);
  test::ExpectEq("bottom zero unitless value", r.value, 0);
}

void TestParseMarginLeftPx() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginLeft("margin-left: 32px;");
  test::ExpectEq("left px unit", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("left px value", r.value, 32);
}

void TestParseMarginRightPercent() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginRight("margin-right: 10%;");
  test::ExpectEq("right percent unit", (int)r.unit, (int)R::Unit::Percent);
  test::ExpectEq("right percent value", r.value, 10);
}

void TestParseMarginLeftShorthand() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginLeft("margin: 8px 16px 24px 32px;");
  test::ExpectEq("left shorthand unit", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("left shorthand value", r.value, 32);
}

void TestParseMarginLeftPercent() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginLeft("margin-left: 10%;");
  test::ExpectEq("left percent unit", (int)r.unit, (int)R::Unit::Percent);
  test::ExpectEq("left percent value", r.value, 10);
}

void TestParseMarginRightPx() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginRight("margin-right: 32px;");
  test::ExpectEq("right px unit", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("right px value", r.value, 32);
}

void TestParseMarginRightShorthand() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginRight("margin: 8px 16px 24px 32px;");
  test::ExpectEq("right shorthand unit", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("right shorthand value", r.value, 16);
}

void TestParseMarginRightShorthandTwoValues() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginRight("margin: 8px 16px;");
  test::ExpectEq("right shorthand 2val unit", (int)r.unit, (int)R::Unit::Px);
  test::ExpectEq("right shorthand 2val value", r.value, 16);
}

void TestResolveHorizontalMarginPx() {
  using R = book_xml_css_style_utils::MarginTopResult;

  R px{};
  px.unit = R::Unit::Px;
  px.value = 32;
  test::ExpectEq("positive px margin",
                 book_xml_css_style_utils::ResolveHorizontalMarginPx(px, 240),
                 32);

  R percent{};
  percent.unit = R::Unit::Percent;
  percent.value = 15;
  test::ExpectEq("percent margin",
                 book_xml_css_style_utils::ResolveHorizontalMarginPx(percent, 240),
                 36);

  R negative_px{};
  negative_px.unit = R::Unit::Px;
  negative_px.value = 10;
  negative_px.negative = true;
  test::ExpectEq("negative px margin",
                 book_xml_css_style_utils::ResolveHorizontalMarginPx(
                     negative_px, 240),
                 -10);

  R negative_percent{};
  negative_percent.unit = R::Unit::Percent;
  negative_percent.value = 20;
  negative_percent.negative = true;
  test::ExpectEq("negative percent margin",
                 book_xml_css_style_utils::ResolveHorizontalMarginPx(
                     negative_percent, 240),
                 -48);
}

void TestTryParseFontSizeAcceptsPxValues() {
  book_xml_css_style_utils::FontSizeSpec spec{};
  const bool ok =
      book_xml_css_style_utils::TryParseFontSize("font-size: 21px;", &spec);
  test::ExpectTrue("font-size px parsed", ok);
  test::ExpectEq("font-size px unit", (int)spec.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Px);
  test::ExpectEq("font-size px value", spec.value_x100, 2100);
}

void TestTryParseFontSizeAcceptsRelativeValues() {
  book_xml_css_style_utils::FontSizeSpec percent{};
  test::ExpectTrue("font-size percent parsed",
                   book_xml_css_style_utils::TryParseFontSize(
                       "font-size: 150%;", &percent));
  test::ExpectEq("font-size percent unit", (int)percent.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Percent);
  test::ExpectEq("font-size percent value", percent.value_x100, 15000);

  book_xml_css_style_utils::FontSizeSpec em{};
  test::ExpectTrue("font-size em parsed",
                   book_xml_css_style_utils::TryParseFontSize(
                       "font-size: 1.4em;", &em));
  test::ExpectEq("font-size em unit", (int)em.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Em);
  test::ExpectEq("font-size em value", em.value_x100, 140);

  book_xml_css_style_utils::FontSizeSpec rem{};
  test::ExpectTrue("font-size rem parsed",
                   book_xml_css_style_utils::TryParseFontSize(
                       "font-size: 1.25rem;", &rem));
  test::ExpectEq("font-size rem unit", (int)rem.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Rem);
  test::ExpectEq("font-size rem value", rem.value_x100, 125);

  book_xml_css_style_utils::FontSizeSpec smaller{};
  test::ExpectTrue("font-size smaller parsed",
                   book_xml_css_style_utils::TryParseFontSize(
                       "font-size: smaller;", &smaller));
  test::ExpectEq("font-size smaller unit", (int)smaller.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Smaller);

  book_xml_css_style_utils::FontSizeSpec larger{};
  test::ExpectTrue("font-size larger parsed",
                   book_xml_css_style_utils::TryParseFontSize(
                       "font-size: larger;", &larger));
  test::ExpectEq("font-size larger unit", (int)larger.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Larger);
}

void TestResolveFontSizePxHandlesRelativeValues() {
  using S = book_xml_css_style_utils::FontSizeSpec;

  S px{};
  px.unit = S::Unit::Px;
  px.value_x100 = 1950;
  test::ExpectEq("px resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(px, 14), 20);

  S percent{};
  percent.unit = S::Unit::Percent;
  percent.value_x100 = 15000;
  test::ExpectEq("percent resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(percent, 14), 21);

  S em{};
  em.unit = S::Unit::Em;
  em.value_x100 = 140;
  test::ExpectEq("em resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(em, 14), 20);

  S rem{};
  rem.unit = S::Unit::Rem;
  rem.value_x100 = 125;
  test::ExpectEq("rem resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(rem, 14), 18);

  S smaller{};
  smaller.unit = S::Unit::Smaller;
  test::ExpectEq("smaller resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(smaller, 14), 12);

  S larger{};
  larger.unit = S::Unit::Larger;
  test::ExpectEq("larger resolved",
                 book_xml_css_style_utils::ResolveFontSizePx(larger, 14), 17);
}

void TestParseTextIndent() {
  using R = book_xml_css_style_utils::MarginTopResult;

  R px = book_xml_css_style_utils::ParseTextIndent("text-indent: 24px;");
  test::ExpectEq("text-indent px -> Px unit", (int)px.unit, (int)R::Unit::Px);
  test::ExpectEq("text-indent px value", px.value, 24);

  R em = book_xml_css_style_utils::ParseTextIndent("text-indent: 2em;");
  test::ExpectEq("text-indent em -> Px unit", (int)em.unit, (int)R::Unit::Px);
  test::ExpectEq("text-indent 2em value (2*12)", em.value, 24);

  R none = book_xml_css_style_utils::ParseTextIndent("color: red;");
  test::ExpectEq("no text-indent -> None", (int)none.unit, (int)R::Unit::None);

  R neg = book_xml_css_style_utils::ParseTextIndent("text-indent: -8px;");
  test::ExpectEq("negative text-indent -> Px", (int)neg.unit, (int)R::Unit::Px);
  test::ExpectTrue("negative text-indent is negative", neg.negative);
}

void TestParseTextAlignStartEnd() {
  using TA = book_xml_css_style_utils::TextAlign;

  TA start = TA::Left;
  test::ExpectTrue("start parsed",
                   book_xml_css_style_utils::TryParseTextAlign(
                       "text-align: start;", &start));
  test::ExpectEq("start maps to Left", (int)start, (int)TA::Left);

  TA end = TA::Left;
  test::ExpectTrue("end parsed",
                   book_xml_css_style_utils::TryParseTextAlign(
                       "text-align: end;", &end));
  test::ExpectEq("end maps to Right", (int)end, (int)TA::Right);
}

void TestParseWhiteSpaceModes() {
  using WS = book_xml_css_style_utils::WhiteSpaceMode;

  WS pre = WS::Normal;
  test::ExpectTrue("pre parsed",
                   book_xml_css_style_utils::TryParseWhiteSpace(
                       "white-space: pre;", &pre));
  test::ExpectEq("pre mode", (int)pre, (int)WS::Pre);

  WS nowrap = WS::Normal;
  test::ExpectTrue("nowrap parsed",
                   book_xml_css_style_utils::TryParseWhiteSpace(
                       "white-space: nowrap;", &nowrap));
  test::ExpectEq("nowrap mode", (int)nowrap, (int)WS::Nowrap);

  WS pre_wrap = WS::Normal;
  test::ExpectTrue("pre-wrap parsed",
                   book_xml_css_style_utils::TryParseWhiteSpace(
                       "white-space: pre-wrap;", &pre_wrap));
  test::ExpectEq("pre-wrap mode", (int)pre_wrap, (int)WS::PreWrap);

  WS pre_line = WS::Normal;
  test::ExpectTrue("pre-line parsed",
                   book_xml_css_style_utils::TryParseWhiteSpace(
                       "white-space: pre-line;", &pre_line));
  test::ExpectEq("pre-line mode", (int)pre_line, (int)WS::PreLine);
}

void TestNormalizeWhiteSpaceText() {
  using WS = book_xml_css_style_utils::WhiteSpaceMode;

  std::string nowrap =
      book_xml_css_style_utils::NormalizeWhiteSpaceText(
          " Alpha\t beta \n gamma  ", 22, WS::Nowrap);
  test::ExpectTrue("nowrap collapses whitespace",
                   nowrap == " Alpha beta gamma ");

  std::string pre_line =
      book_xml_css_style_utils::NormalizeWhiteSpaceText(
          " Alpha\t beta \n gamma \n\n delta ", 31, WS::PreLine);
  test::ExpectTrue("pre-line preserves newlines and collapses spaces",
                   pre_line == " Alpha beta\ngamma\n\ndelta");
}

void TestParseTextTransform() {
  using TT = book_xml_css_style_utils::TextTransform;

  test::ExpectEq("uppercase",
                 (int)book_xml_css_style_utils::ParseTextTransform(
                     "text-transform: uppercase;"),
                 (int)TT::Uppercase);
  test::ExpectEq("lowercase",
                 (int)book_xml_css_style_utils::ParseTextTransform(
                     "text-transform: lowercase;"),
                 (int)TT::Lowercase);
  test::ExpectEq("capitalize",
                 (int)book_xml_css_style_utils::ParseTextTransform(
                     "text-transform: capitalize;"),
                 (int)TT::Capitalize);
  test::ExpectEq("none keyword",
                 (int)book_xml_css_style_utils::ParseTextTransform(
                     "text-transform: none;"),
                 (int)TT::None);
  test::ExpectEq("missing property",
                 (int)book_xml_css_style_utils::ParseTextTransform("color: red;"),
                 (int)TT::None);
}

void TestParsePageBreakInsideAvoid() {
  test::ExpectTrue("page-break-inside avoid parsed",
                   book_xml_css_style_utils::HasPageBreakInsideAvoid(
                       "page-break-inside: avoid;"));
  test::ExpectTrue("break-inside avoid parsed",
                   book_xml_css_style_utils::HasPageBreakInsideAvoid(
                       "break-inside: avoid;"));
  test::ExpectFalse("auto not parsed as avoid",
                    book_xml_css_style_utils::HasPageBreakInsideAvoid(
                        "page-break-inside: auto;"));
}

void TestParsePageBreakBeforeAfter() {
  test::ExpectTrue("page-break-before always parsed",
                   book_xml_css_style_utils::HasPageBreakBefore(
                       "page-break-before: always;"));
  test::ExpectTrue("break-before page parsed",
                   book_xml_css_style_utils::HasPageBreakBefore(
                       "break-before: page;"));
  test::ExpectFalse("break-before auto ignored",
                    book_xml_css_style_utils::HasPageBreakBefore(
                        "break-before: auto;"));

  test::ExpectTrue("page-break-after always parsed",
                   book_xml_css_style_utils::HasPageBreakAfter(
                       "page-break-after: always;"));
  test::ExpectTrue("break-after page parsed",
                   book_xml_css_style_utils::HasPageBreakAfter(
                       "break-after: page;"));
  test::ExpectFalse("break-after auto ignored",
                    book_xml_css_style_utils::HasPageBreakAfter(
                        "break-after: auto;"));
}

void TestParseFloatAndClear() {
  using FM = book_xml_css_style_utils::FloatMode;
  using CM = book_xml_css_style_utils::ClearMode;

  FM left = FM::None;
  test::ExpectTrue("float left parsed",
                   book_xml_css_style_utils::TryParseFloat(
                       "float: left;", &left));
  test::ExpectEq("float left value", (int)left, (int)FM::Left);

  FM right = FM::None;
  test::ExpectTrue("float right parsed",
                   book_xml_css_style_utils::TryParseFloat(
                       "float:right;", &right));
  test::ExpectEq("float right value", (int)right, (int)FM::Right);

  CM both = CM::None;
  test::ExpectTrue("clear both parsed",
                   book_xml_css_style_utils::TryParseClear(
                       "clear: both;", &both));
  test::ExpectEq("clear both value", (int)both, (int)CM::Both);

  CM left_clear = CM::None;
  test::ExpectTrue("clear left parsed",
                   book_xml_css_style_utils::TryParseClear(
                       "clear:left;", &left_clear));
  test::ExpectEq("clear left value", (int)left_clear, (int)CM::Left);
}

} // namespace

int main() {
  TestDetectsOverlineDecoration();
  TestDetectsUnderlineRegardlessOfDecorationOrder();
  TestDetectsDashedAndDottedUnderlineStyles();
  TestDetectsDoubleUnderlineStyle();
  TestDetectsBoldItalicAndVerticalAlign();
  TestParseMarginTopPositivePx();
  TestParseMarginTopNegativePx();
  TestParseMarginTopPercent();
  TestParseMarginTopZeroUnitless();
  TestParseMarginTopZeroPx();
  TestParseMarginTopMissingProperty();
  TestParseMarginTopNull();
  TestParseMarginTopEmUnit();
  TestParseMarginTopPtUnit();
  TestParseMarginBottomZeroUnitless();
  TestParseMarginLeftPx();
  TestParseMarginLeftPercent();
  TestParseMarginLeftShorthand();
  TestParseMarginRightPx();
  TestParseMarginRightPercent();
  TestParseMarginRightShorthand();
  TestParseMarginRightShorthandTwoValues();
  TestResolveHorizontalMarginPx();
  TestTryParseFontSizeAcceptsPxValues();
  TestTryParseFontSizeAcceptsRelativeValues();
  TestTryParseFontSizePt();
  TestTryParseFontSizeAbsoluteKeywords();
  TestResolveFontSizePxHandlesRelativeValues();
  TestParseInlineFlagsResets();
  TestParseTextIndent();
  TestParseTextAlignStartEnd();
  TestParseWhiteSpaceModes();
  TestNormalizeWhiteSpaceText();
  TestParseTextTransform();
  TestParsePageBreakInsideAvoid();
  TestParsePageBreakBeforeAfter();
  TestParseFloatAndClear();
  return 0;
}
