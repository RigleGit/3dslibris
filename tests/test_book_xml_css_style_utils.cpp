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
  test::ExpectEq("em -> None", (int)r.unit, (int)R::Unit::None);
}

void TestParseMarginBottomZeroUnitless() {
  using R = book_xml_css_style_utils::MarginTopResult;
  R r = book_xml_css_style_utils::ParseMarginBottom("margin-bottom: 0;");
  test::ExpectEq("bottom zero unitless -> Px", (int)r.unit,
                 (int)R::Unit::Px);
  test::ExpectEq("bottom zero unitless value", r.value, 0);
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

} // namespace

int main() {
  TestDetectsOverlineDecoration();
  TestDetectsUnderlineRegardlessOfDecorationOrder();
  TestDetectsDashedAndDottedUnderlineStyles();
  TestDetectsBoldItalicAndVerticalAlign();
  TestParseMarginTopPositivePx();
  TestParseMarginTopNegativePx();
  TestParseMarginTopPercent();
  TestParseMarginTopZeroUnitless();
  TestParseMarginTopZeroPx();
  TestParseMarginTopMissingProperty();
  TestParseMarginTopNull();
  TestParseMarginTopEmUnit();
  TestParseMarginBottomZeroUnitless();
  TestTryParseFontSizeAcceptsPxValues();
  TestTryParseFontSizeAcceptsRelativeValues();
  TestResolveFontSizePxHandlesRelativeValues();
  return 0;
}
