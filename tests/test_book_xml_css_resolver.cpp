#include "book/book_xml_css_resolver.h"
#include "book/book_xml_css_style_utils.h"
#include "book/epub_css_class_map.h"
#include "test_assert.h"

namespace {

void TestExtractStyleAttr() {
  const char *attr[] = {"class", "foo", "style", "color:red", nullptr};
  test::ExpectStrEq("extract style",
                    book_xml_css_resolver::ExtractStyleAttr(attr).c_str(),
                    "color:red");

  const char *no_style[] = {"class", "foo", nullptr};
  test::ExpectStrEq("missing style",
                    book_xml_css_resolver::ExtractStyleAttr(no_style).c_str(),
                    "");

  test::ExpectStrEq("null attr",
                    book_xml_css_resolver::ExtractStyleAttr(nullptr).c_str(),
                    "");
}

void TestExtractClassAttr() {
  const char *attr[] = {"style", "color:red", "class", "chapter", nullptr};
  test::ExpectStrEq("extract class",
                    book_xml_css_resolver::ExtractClassAttr(attr).c_str(),
                    "chapter");

  const char *no_class[] = {"style", "color:red", nullptr};
  test::ExpectStrEq("missing class",
                    book_xml_css_resolver::ExtractClassAttr(no_class).c_str(),
                    "");
}

void TestParseCssLengthPxUnits() {
  test::ExpectEq("px", book_xml_css_resolver::ParseCssLengthPx("10px", 240, 14), 10);
  test::ExpectEq("bare int", book_xml_css_resolver::ParseCssLengthPx("20", 240, 14), 20);
  test::ExpectEq("50 percent of 240",
                 book_xml_css_resolver::ParseCssLengthPx("50%", 240, 14), 120);
  test::ExpectEq("1em at 14px", book_xml_css_resolver::ParseCssLengthPx("1em", 240, 14), 14);
  test::ExpectEq("2rem at 14px", book_xml_css_resolver::ParseCssLengthPx("2rem", 240, 14), 28);
  test::ExpectEq("empty string", book_xml_css_resolver::ParseCssLengthPx("", 240, 14), 0);
  test::ExpectEq("null", book_xml_css_resolver::ParseCssLengthPx(nullptr, 240, 14), 0);
  test::ExpectEq("unknown unit", book_xml_css_resolver::ParseCssLengthPx("5ch", 240, 14), 0);
  test::ExpectEq("capped to text_width",
                 book_xml_css_resolver::ParseCssLengthPx("500px", 240, 14), 240);
}

void TestParseCssLengthPxFractional() {
  test::ExpectEq("1.5em at 14px",
                 book_xml_css_resolver::ParseCssLengthPx("1.5em", 240, 14), 21);
  test::ExpectEq("10.5px rounds",
                 book_xml_css_resolver::ParseCssLengthPx("10.5px", 240, 14), 11);
}

void TestParseImgWidthPx() {
  test::ExpectEq("width attr px",
                 book_xml_css_resolver::ParseImgWidthPx("80", nullptr, 240, 14), 80);
  test::ExpectEq("width attr percent",
                 book_xml_css_resolver::ParseImgWidthPx("50%", nullptr, 240, 14), 120);
  test::ExpectEq("style wins over null attr",
                 book_xml_css_resolver::ParseImgWidthPx(nullptr, "width: 60px", 240, 14), 60);
  test::ExpectEq("attr wins over style",
                 book_xml_css_resolver::ParseImgWidthPx("40", "width: 60px", 240, 14), 40);
  test::ExpectEq("no width",
                 book_xml_css_resolver::ParseImgWidthPx(nullptr, nullptr, 240, 14), 0);
}

void TestParseImgWidthPxUsesRootFontForRelativeUnits() {
  test::ExpectEq("image em width uses root font",
                 book_xml_css_resolver::ParseImgWidthPx("10em", nullptr, 240,
                                                        28, 14),
                 140);
  test::ExpectEq("image rem width uses root font",
                 book_xml_css_resolver::ParseImgWidthPx(nullptr, "width: 8rem",
                                                        240, 28, 14),
                 112);
}

void TestParseInlineHiddenFlags() {
  bool h = false;
  book_xml_css_resolver::ParseInlineHiddenFlags("display:none", &h);
  test::ExpectTrue("display:none hidden", h);

  h = false;
  book_xml_css_resolver::ParseInlineHiddenFlags("display: none", &h);
  test::ExpectTrue("display: none hidden", h);

  h = false;
  book_xml_css_resolver::ParseInlineHiddenFlags("visibility:hidden", &h);
  test::ExpectTrue("visibility:hidden", h);

  h = false;
  book_xml_css_resolver::ParseInlineHiddenFlags("color:red", &h);
  test::ExpectFalse("color:red not hidden", h);

  h = false;
  book_xml_css_resolver::ParseInlineHiddenFlags(
      "width:1px;height:1px;position:absolute;overflow:hidden", &h);
  test::ExpectTrue("1x1 offscreen hidden", h);
}

void TestParseClassHiddenFlags() {
  bool h = false;
  book_xml_css_resolver::ParseClassHiddenFlags("sr-only", &h);
  test::ExpectTrue("sr-only hidden", h);

  h = false;
  book_xml_css_resolver::ParseClassHiddenFlags("visually-hidden", &h);
  test::ExpectTrue("visually-hidden", h);

  h = false;
  book_xml_css_resolver::ParseClassHiddenFlags("chapter-title", &h);
  test::ExpectFalse("chapter-title not hidden", h);
}

void TestParseElementHiddenFlags() {
  bool h = false;
  const char *attr_hidden[] = {"hidden", "", nullptr};
  book_xml_css_resolver::ParseElementHiddenFlags(attr_hidden, &h);
  test::ExpectTrue("hidden attr", h);

  h = false;
  const char *attr_aria[] = {"aria-hidden", "true", nullptr};
  book_xml_css_resolver::ParseElementHiddenFlags(attr_aria, &h);
  test::ExpectTrue("aria-hidden true", h);

  h = false;
  const char *attr_style[] = {"style", "display:none", nullptr};
  book_xml_css_resolver::ParseElementHiddenFlags(attr_style, &h);
  test::ExpectTrue("style display:none", h);

  h = false;
  const char *attr_class[] = {"class", "sr-only", nullptr};
  book_xml_css_resolver::ParseElementHiddenFlags(attr_class, &h);
  test::ExpectTrue("class sr-only", h);

  h = false;
  const char *attr_none[] = {"id", "main", nullptr};
  book_xml_css_resolver::ParseElementHiddenFlags(attr_none, &h);
  test::ExpectFalse("no hidden attr", h);
}

void TestStyleLooksDisplayBlock() {
  test::ExpectTrue("display:block",
                   book_xml_css_resolver::StyleLooksDisplayBlock("display:block"));
  test::ExpectTrue("display: block",
                   book_xml_css_resolver::StyleLooksDisplayBlock("display: block"));
  test::ExpectFalse("display:inline false",
                    book_xml_css_resolver::StyleLooksDisplayBlock("display:inline"));
  test::ExpectFalse("empty false",
                    book_xml_css_resolver::StyleLooksDisplayBlock(""));
}

void TestElementCanCarryBlockTextAlign() {
  test::ExpectTrue("div",
                   book_xml_css_resolver::ElementCanCarryBlockTextAlign("div", ""));
  test::ExpectTrue("body",
                   book_xml_css_resolver::ElementCanCarryBlockTextAlign("body", ""));
  test::ExpectTrue("section",
                   book_xml_css_resolver::ElementCanCarryBlockTextAlign("section", ""));
  test::ExpectFalse("span no",
                    book_xml_css_resolver::ElementCanCarryBlockTextAlign("span", ""));
  test::ExpectTrue("span with display:block yes",
                   book_xml_css_resolver::ElementCanCarryBlockTextAlign(
                       "span", "display:block"));
}

void TestResolveElementTextAlignWithClass() {
  epub_css_class_map::CssClassMap class_map;

  bool stack[4] = {false, false, false, false};
  uint8_t stack_vals[4] = {0, 0, 0, 0};

  test::ExpectEq(
      "inline style right",
      (int)book_xml_css_resolver::ResolveElementTextAlignWithClass(
          "text-align:right", "", stack, stack_vals, 0, class_map),
      (int)book_xml_css_style_utils::TextAlign::Right);

  class_map["centered"].text_align = book_xml_css_style_utils::TextAlign::Center;
  class_map["centered"].has_text_align = true;

  test::ExpectEq(
      "class center",
      (int)book_xml_css_resolver::ResolveElementTextAlignWithClass(
          "", "centered", stack, stack_vals, 0, class_map),
      (int)book_xml_css_style_utils::TextAlign::Center);

  stack[0] = true;
  stack_vals[0] = (uint8_t)book_xml_css_style_utils::TextAlign::Right;

  test::ExpectEq(
      "inherited right from stack",
      (int)book_xml_css_resolver::ResolveElementTextAlignWithClass(
          "", "", stack, stack_vals, 1, class_map),
      (int)book_xml_css_style_utils::TextAlign::Right);

  test::ExpectEq(
      "inline style beats class and stack",
      (int)book_xml_css_resolver::ResolveElementTextAlignWithClass(
          "text-align:left", "centered", stack, stack_vals, 1, class_map),
      (int)book_xml_css_style_utils::TextAlign::Left);
}

void TestParseElementMarginTopPx() {
  const char *attr_style[] = {"style", "margin-top:24px", nullptr};
  auto r = book_xml_css_resolver::ParseElementMarginTopPx(attr_style);
  test::ExpectTrue("margin-top 24px unit px",
                   r.unit == book_xml_css_style_utils::MarginTopResult::Unit::Px);
  test::ExpectEq("margin-top 24px value", r.value, 24);

  const char *attr_none[] = {"class", "foo", nullptr};
  auto r2 = book_xml_css_resolver::ParseElementMarginTopPx(attr_none);
  test::ExpectTrue("no style attr returns None",
                   r2.unit == book_xml_css_style_utils::MarginTopResult::Unit::None);
}

void TestParseElementMarginBottomWithClass() {
  epub_css_class_map::CssClassMap class_map;

  auto r = book_xml_css_resolver::ParseElementMarginBottomWithClass(
      "margin-bottom:16px", "", class_map);
  test::ExpectTrue("inline style margin-bottom px",
                   r.unit == book_xml_css_style_utils::MarginTopResult::Unit::Px);
  test::ExpectEq("inline style margin-bottom value", r.value, 16);

  class_map["mb24"].margin_bottom.unit =
      book_xml_css_style_utils::MarginTopResult::Unit::Px;
  class_map["mb24"].margin_bottom.value = 24;

  auto r2 = book_xml_css_resolver::ParseElementMarginBottomWithClass(
      "", "mb24", class_map);
  test::ExpectTrue("class margin-bottom px",
                   r2.unit == book_xml_css_style_utils::MarginTopResult::Unit::Px);
  test::ExpectEq("class margin-bottom value", r2.value, 24);

  auto r3 = book_xml_css_resolver::ParseElementMarginBottomWithClass(
      "margin-bottom:8px", "mb24", class_map);
  test::ExpectEq("inline beats class", r3.value, 8);
}

void TestParseElementStyleFlagsFromStyle() {
  bool bold = false, italic = false, underline = false;
  uint8_t ul_style = 0;
  bool overline = false, strike = false, sup = false, sub = false;
  bool no_ul = false, reset_bold = false, reset_italic = false;

  const char *attr[] = {
      "style", "font-weight:bold;font-style:italic;text-decoration:underline",
      nullptr};
  book_xml_css_resolver::ParseElementStyleFlags(
      attr, &bold, &italic, &underline, &ul_style, &overline, &strike, &sup,
      &sub, &no_ul, &reset_bold, &reset_italic);

  test::ExpectTrue("style bold", bold);
  test::ExpectTrue("style italic", italic);
  test::ExpectTrue("style underline", underline);
}

void TestParseElementStyleFlagsFromClass() {
  bool bold = false, italic = false;
  const char *attr[] = {"class", "boldtext", nullptr};
  book_xml_css_resolver::ParseElementStyleFlags(attr, &bold, &italic, nullptr,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr);
  test::ExpectTrue("class bold", bold);
}

} // namespace

int main() {
  TestExtractStyleAttr();
  TestExtractClassAttr();
  TestParseCssLengthPxUnits();
  TestParseCssLengthPxFractional();
  TestParseImgWidthPx();
  TestParseImgWidthPxUsesRootFontForRelativeUnits();
  TestParseInlineHiddenFlags();
  TestParseClassHiddenFlags();
  TestParseElementHiddenFlags();
  TestStyleLooksDisplayBlock();
  TestElementCanCarryBlockTextAlign();
  TestResolveElementTextAlignWithClass();
  TestParseElementMarginTopPx();
  TestParseElementMarginBottomWithClass();
  TestParseElementStyleFlagsFromStyle();
  TestParseElementStyleFlagsFromClass();
  return 0;
}
