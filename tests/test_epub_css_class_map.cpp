#include "book/epub_css_class_map.h"

#include "test_assert.h"

namespace {

using epub_css_class_map::CssClassMap;
using epub_css_class_map::CssClassMargins;

void ExpectMarginTopPx(const char *label, const CssClassMargins &m, int value) {
  test::ExpectEq((std::string(label) + " top unit").c_str(),
                 (int)m.margin_top.unit,
                 (int)book_xml_css_style_utils::MarginTopResult::Unit::Px);
  test::ExpectEq((std::string(label) + " top value").c_str(),
                 m.margin_top.value, value);
}

void ExpectMarginBottomPercent(const char *label, const CssClassMargins &m,
                               int value) {
  test::ExpectEq((std::string(label) + " bottom unit").c_str(),
                 (int)m.margin_bottom.unit,
                 (int)book_xml_css_style_utils::MarginTopResult::Unit::Percent);
  test::ExpectEq((std::string(label) + " bottom value").c_str(),
                 m.margin_bottom.value, value);
}

void TestParseCssIntoClassMapSupportsQualifiedAndGroupedSelectors() {
  const char *css =
      ".mt-inline { margin-top: 24px; }\n"
      "p.mb-class { margin-bottom: 8%; }\n"
      ".align-center { text-align: center; }\n"
      ".title { font-size: 21px; }\n"
      ".mt-group, h2.mt-heading { margin-top: 12px; }\n"
      "div.note strong { margin-top: 99px; }\n"
      ".combo.one { margin-bottom: 77px; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectEq("parsed class count", (int)out.size(), 6);
  test::ExpectTrue("simple class parsed", out.find("mt-inline") != out.end());
  test::ExpectTrue("element qualified class parsed",
                   out.find("mb-class") != out.end());
  test::ExpectTrue("grouped selector class parsed",
                   out.find("mt-group") != out.end());
  test::ExpectTrue("grouped selector qualified class parsed",
                   out.find("mt-heading") != out.end());
  test::ExpectTrue("align class parsed", out.find("align-center") != out.end());
  test::ExpectTrue("font-size class parsed", out.find("title") != out.end());
  test::ExpectTrue("descendant selector ignored",
                   out.find("note") == out.end());
  test::ExpectTrue("compound class selector ignored",
                   out.find("combo") == out.end() && out.find("one") == out.end());

  ExpectMarginTopPx("mt-inline", out["mt-inline"], 24);
  ExpectMarginBottomPercent("mb-class", out["mb-class"], 8);
  ExpectMarginTopPx("mt-group", out["mt-group"], 12);
  ExpectMarginTopPx("mt-heading", out["mt-heading"], 12);
  test::ExpectTrue("align-center keeps text-align",
                   out["align-center"].has_text_align);
  test::ExpectEq("align-center value",
                 (int)out["align-center"].text_align,
                 (int)book_xml_css_style_utils::TextAlign::Center);
  test::ExpectEq("title keeps font-size unit", (int)out["title"].font_size.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Px);
  test::ExpectEq("title font-size value", out["title"].font_size.value_x100, 2100);
}

void TestLookupMarginsForClassAttrMergesKnownClasses() {
  CssClassMap map;
  map["mt-class"].margin_top.value = 18;
  map["mt-class"].margin_top.unit =
      book_xml_css_style_utils::MarginTopResult::Unit::Px;
  map["mb-class"].margin_bottom.value = 6;
  map["mb-class"].margin_bottom.unit =
      book_xml_css_style_utils::MarginTopResult::Unit::Percent;

  CssClassMargins out;
  const bool found = epub_css_class_map::LookupMarginsForClassAttr(
      "  ignored mt-class  mb-class ", map, &out);

  test::ExpectTrue("found at least one known class", found);
  ExpectMarginTopPx("merged top", out, 18);
  ExpectMarginBottomPercent("merged bottom", out, 6);
}

void TestLookupMarginsForClassAttrRejectsUnknownClasses() {
  CssClassMap map;
  CssClassMargins out;
  const bool found = epub_css_class_map::LookupMarginsForClassAttr(
      "foo bar", map, &out);
  test::ExpectFalse("no classes found", found);
}

void TestParseCssIntoClassMapDetectsListStyleNone() {
  const char *css =
      ".ornamentless { list-style-type: none; }\n"
      "ol.clean { list-style: none; }\n"
      ".normal-list { list-style-type: disc; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("ornamentless parsed", out.find("ornamentless") != out.end());
  test::ExpectTrue("qualified clean parsed", out.find("clean") != out.end());
  test::ExpectTrue("ornamentless hides markers",
                   out["ornamentless"].hide_list_markers);
  test::ExpectTrue("clean hides markers", out["clean"].hide_list_markers);
  test::ExpectFalse("normal list keeps markers",
                    out["normal-list"].hide_list_markers);
}

void TestLookupTextAlignForClassAttrUsesLastKnownMatch() {
  CssClassMap map;
  map["align-left"].has_text_align = true;
  map["align-left"].text_align = book_xml_css_style_utils::TextAlign::Left;
  map["align-right"].has_text_align = true;
  map["align-right"].text_align = book_xml_css_style_utils::TextAlign::Right;

  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Center;
  const bool found = epub_css_class_map::LookupTextAlignForClassAttr(
      "align-left align-right", map, &align);
  test::ExpectTrue("found text align", found);
  test::ExpectEq("last matching class wins", (int)align,
                 (int)book_xml_css_style_utils::TextAlign::Right);
}

void TestLookupFontSizeForClassAttrUsesLastKnownMatch() {
  CssClassMap map;
  map["small"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Percent;
  map["small"].font_size.value_x100 = 9000;
  map["large"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Em;
  map["large"].font_size.value_x100 = 140;

  book_xml_css_style_utils::FontSizeSpec spec{};
  const bool found = epub_css_class_map::LookupFontSizeForClassAttr(
      "small large", map, &spec);
  test::ExpectTrue("found font-size", found);
  test::ExpectEq("last matching font-size unit", (int)spec.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Em);
  test::ExpectEq("last matching font-size wins", spec.value_x100, 140);
}

void TestParseCssIntoClassMapDetectsSuperSubScript() {
  const char *css =
      ".sup { vertical-align: super; }\n"
      "span.sub { vertical-align:sub; }\n"
      ".normal { color: red; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("sup class parsed", out.find("sup") != out.end());
  test::ExpectTrue("sub class parsed", out.find("sub") != out.end());
  test::ExpectFalse("normal class not parsed for super/sub",
                    out.count("normal") && out.at("normal").superscript);
  test::ExpectTrue("sup has superscript flag", out["sup"].superscript);
  test::ExpectFalse("sup not subscript", out["sup"].subscript);
  test::ExpectTrue("sub has subscript flag", out["sub"].subscript);
  test::ExpectFalse("sub not superscript", out["sub"].superscript);
}

void TestLookupSuperSubForClassAttr() {
  CssClassMap map;
  map["sup"].superscript = true;
  map["sub"].subscript = true;

  bool is_super = false, is_sub = false;
  const bool found = epub_css_class_map::LookupSuperSubForClassAttr(
      "sup", map, &is_super, &is_sub);
  test::ExpectTrue("found superscript class", found);
  test::ExpectTrue("superscript set", is_super);
  test::ExpectFalse("subscript not set", is_sub);

  is_super = false;
  is_sub = false;
  const bool found2 = epub_css_class_map::LookupSuperSubForClassAttr(
      "unknown sub", map, &is_super, &is_sub);
  test::ExpectTrue("found subscript class", found2);
  test::ExpectFalse("superscript not set", is_super);
  test::ExpectTrue("subscript set", is_sub);

  is_super = false;
  is_sub = false;
  const bool found3 = epub_css_class_map::LookupSuperSubForClassAttr(
      "unknown", map, &is_super, &is_sub);
  test::ExpectFalse("no match found", found3);
}

void TestParseCssIntoClassMapDetectsResetFlags() {
  const char *css =
      ".no-underline { text-decoration: none; }\n"
      ".normal-weight { font-weight: normal; }\n"
      ".light { font-weight: 300; }\n"
      ".normal-style { font-style: normal; }\n"
      ".still-bold { font-weight: bold; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("no-underline class parsed", out.count("no-underline") > 0);
  test::ExpectTrue("no-underline flag set", out["no-underline"].no_underline);
  test::ExpectFalse("no-underline does not set reset_bold",
                    out["no-underline"].reset_bold);

  test::ExpectTrue("normal-weight class parsed", out.count("normal-weight") > 0);
  test::ExpectTrue("normal-weight reset_bold set",
                   out["normal-weight"].reset_bold);

  test::ExpectTrue("light class parsed", out.count("light") > 0);
  test::ExpectTrue("light reset_bold set", out["light"].reset_bold);

  test::ExpectTrue("normal-style class parsed", out.count("normal-style") > 0);
  test::ExpectTrue("normal-style reset_italic set",
                   out["normal-style"].reset_italic);

  // font-weight: bold has no CssClassMargins slot; class map won't contain it.
  // Verify LookupResetBoldForClassAttr returns false for that class attr.
  test::ExpectFalse("still-bold not in reset_bold lookup",
                    epub_css_class_map::LookupResetBoldForClassAttr(
                        "still-bold", out));
}

void TestLookupResetFlagFunctions() {
  CssClassMap map;
  map["nd"].no_underline = true;
  map["rb"].reset_bold = true;
  map["ri"].reset_italic = true;

  test::ExpectTrue("LookupNoUnderline finds class",
                   epub_css_class_map::LookupNoUnderlineForClassAttr("nd", map));
  test::ExpectFalse("LookupNoUnderline misses unrelated class",
                    epub_css_class_map::LookupNoUnderlineForClassAttr("rb", map));

  test::ExpectTrue("LookupResetBold finds class",
                   epub_css_class_map::LookupResetBoldForClassAttr("rb", map));
  test::ExpectFalse("LookupResetBold misses unrelated class",
                    epub_css_class_map::LookupResetBoldForClassAttr("nd", map));

  test::ExpectTrue("LookupResetItalic finds class",
                   epub_css_class_map::LookupResetItalicForClassAttr("ri", map));
  test::ExpectFalse("LookupResetItalic misses unrelated class",
                    epub_css_class_map::LookupResetItalicForClassAttr("rb", map));

  test::ExpectFalse("LookupResetBold unknown class returns false",
                    epub_css_class_map::LookupResetBoldForClassAttr("unknown", map));
}

} // namespace

int main() {
  TestParseCssIntoClassMapSupportsQualifiedAndGroupedSelectors();
  TestLookupMarginsForClassAttrMergesKnownClasses();
  TestLookupMarginsForClassAttrRejectsUnknownClasses();
  TestParseCssIntoClassMapDetectsListStyleNone();
  TestLookupTextAlignForClassAttrUsesLastKnownMatch();
  TestLookupFontSizeForClassAttrUsesLastKnownMatch();
  TestParseCssIntoClassMapDetectsSuperSubScript();
  TestLookupSuperSubForClassAttr();
  TestParseCssIntoClassMapDetectsResetFlags();
  TestLookupResetFlagFunctions();
  return 0;
}
