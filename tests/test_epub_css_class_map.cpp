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

void TestParseCssIntoClassMapDetectsTextAlignStartEnd() {
  const char *css =
      ".align-start { text-align: start; }\n"
      ".align-end { text-align: end; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("align-start parsed", out.find("align-start") != out.end());
  test::ExpectTrue("align-end parsed", out.find("align-end") != out.end());
  test::ExpectTrue("align-start has_text_align",
                   out["align-start"].has_text_align);
  test::ExpectTrue("align-end has_text_align",
                   out["align-end"].has_text_align);
  test::ExpectEq("align-start value",
                 (int)out["align-start"].text_align,
                 (int)book_xml_css_style_utils::TextAlign::Left);
  test::ExpectEq("align-end value",
                 (int)out["align-end"].text_align,
                 (int)book_xml_css_style_utils::TextAlign::Right);
}

void TestParseCssIntoClassMapDetectsWhiteSpaceModes() {
  const char *css =
      ".pre { white-space: pre; }\n"
      ".nowrap { white-space: nowrap; }\n"
      ".pre-wrap { white-space: pre-wrap; }\n"
      ".pre-line { white-space: pre-line; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("pre parsed", out.find("pre") != out.end());
  test::ExpectTrue("nowrap parsed", out.find("nowrap") != out.end());
  test::ExpectTrue("pre-wrap parsed", out.find("pre-wrap") != out.end());
  test::ExpectTrue("pre-line parsed", out.find("pre-line") != out.end());

  test::ExpectTrue("pre has white-space", out["pre"].has_white_space);
  test::ExpectTrue("nowrap has white-space", out["nowrap"].has_white_space);
  test::ExpectTrue("pre-wrap has white-space", out["pre-wrap"].has_white_space);
  test::ExpectTrue("pre-line has white-space", out["pre-line"].has_white_space);

  test::ExpectEq("pre white-space value", (int)out["pre"].white_space,
                 (int)book_xml_css_style_utils::WhiteSpaceMode::Pre);
  test::ExpectEq("nowrap white-space value", (int)out["nowrap"].white_space,
                 (int)book_xml_css_style_utils::WhiteSpaceMode::Nowrap);
  test::ExpectEq("pre-wrap white-space value",
                 (int)out["pre-wrap"].white_space,
                 (int)book_xml_css_style_utils::WhiteSpaceMode::PreWrap);
  test::ExpectEq("pre-line white-space value",
                 (int)out["pre-line"].white_space,
                 (int)book_xml_css_style_utils::WhiteSpaceMode::PreLine);
}

void TestParseCssIntoClassMapDetectsPageBreakInsideAvoid() {
  const char *css =
      ".keep { page-break-inside: avoid; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("keep parsed", out.find("keep") != out.end());
  test::ExpectTrue("keep has avoid", out["keep"].page_break_inside_avoid);

  CssClassMap map;
  map["keep"].page_break_inside_avoid = true;
  test::ExpectTrue("lookup page-break-inside avoid",
                   epub_css_class_map::LookupPageBreakInsideAvoidForClassAttr(
                       "keep", map));
}

void TestParseCssIntoClassMapDetectsPageBreakBeforeAfter() {
  const char *css =
      ".chapter-start { page-break-before: always; }\n"
      ".section-start { break-before: page; }\n"
      ".section-end { page-break-after: always; }\n"
      ".css3-end { break-after: page; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("chapter-start parsed", out.find("chapter-start") != out.end());
  test::ExpectTrue("section-start parsed", out.find("section-start") != out.end());
  test::ExpectTrue("section-end parsed", out.find("section-end") != out.end());
  test::ExpectTrue("css3-end parsed", out.find("css3-end") != out.end());

  test::ExpectTrue("chapter-start before",
                   epub_css_class_map::LookupPageBreakBeforeForClassAttr(
                       "chapter-start", out));
  test::ExpectTrue("section-start before",
                   epub_css_class_map::LookupPageBreakBeforeForClassAttr(
                       "section-start", out));
  test::ExpectTrue("section-end after",
                   epub_css_class_map::LookupPageBreakAfterForClassAttr(
                       "section-end", out));
  test::ExpectTrue("css3-end after",
                   epub_css_class_map::LookupPageBreakAfterForClassAttr(
                       "css3-end", out));
  test::ExpectFalse("missing before",
                    epub_css_class_map::LookupPageBreakBeforeForClassAttr(
                        "missing", out));
  test::ExpectFalse("missing after",
                    epub_css_class_map::LookupPageBreakAfterForClassAttr(
                        "missing", out));
}

void TestParseCssIntoClassMapDetectsFloatAndClear() {
  const char *css =
      ".flt-left { float: left; }\n"
      ".flt-right { float: right; }\n"
      ".clr-both { clear: both; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("float-left parsed", out.find("flt-left") != out.end());
  test::ExpectTrue("float-right parsed", out.find("flt-right") != out.end());
  test::ExpectTrue("clear-both parsed", out.find("clr-both") != out.end());

  test::ExpectTrue("float-left has float", out["flt-left"].has_float);
  test::ExpectTrue("float-right has float", out["flt-right"].has_float);
  test::ExpectTrue("clear-both has clear", out["clr-both"].has_clear);

  test::ExpectEq("float-left value", (int)out["flt-left"].float_mode,
                 (int)book_xml_css_style_utils::FloatMode::Left);
  test::ExpectEq("float-right value", (int)out["flt-right"].float_mode,
                 (int)book_xml_css_style_utils::FloatMode::Right);
  test::ExpectEq("clear-both value", (int)out["clr-both"].clear_mode,
                 (int)book_xml_css_style_utils::ClearMode::Both);
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

void TestParseCssIntoClassMapDetectsTextIndentAndTransform() {
  using book_xml_css_style_utils::MarginTopResult;
  using book_xml_css_style_utils::TextTransform;
  const char *css =
      ".indented { text-indent: 24px; }\n"
      ".upper { text-transform: uppercase; }\n"
      ".lower { text-transform: lowercase; }\n"
      ".cap { text-transform: capitalize; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectTrue("indented class parsed", out.count("indented") > 0);
  test::ExpectEq("indented text_indent unit",
                 (int)out["indented"].text_indent.unit,
                 (int)MarginTopResult::Unit::Px);
  test::ExpectEq("indented text_indent value", out["indented"].text_indent.value, 24);

  test::ExpectTrue("upper class parsed", out.count("upper") > 0);
  test::ExpectTrue("upper has_text_transform", out["upper"].has_text_transform);
  test::ExpectEq("upper transform value",
                 (int)out["upper"].text_transform,
                 (int)TextTransform::Uppercase);

  test::ExpectTrue("lower class parsed", out.count("lower") > 0);
  test::ExpectEq("lower transform value",
                 (int)out["lower"].text_transform,
                 (int)TextTransform::Lowercase);

  test::ExpectTrue("cap class parsed", out.count("cap") > 0);
  test::ExpectEq("cap transform value",
                 (int)out["cap"].text_transform,
                 (int)TextTransform::Capitalize);
}

void TestParseCssIntoClassMapDetectsMarginLeftRight() {
  const char *css =
      ".indent-left { margin-left: 24px; }\n"
      ".indent-right { margin-right: 10%; }\n"
      ".indent-both { margin-left: 8px; margin-right: 8px; }\n";

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  test::ExpectEq("parsed margin-left/right class count", (int)out.size(), 3);
  test::ExpectTrue("indent-left parsed", out.count("indent-left") > 0);
  test::ExpectTrue("indent-right parsed", out.count("indent-right") > 0);
  test::ExpectTrue("indent-both parsed", out.count("indent-both") > 0);

  using U = book_xml_css_style_utils::MarginTopResult::Unit;
  test::ExpectEq("indent-left unit", (int)out["indent-left"].margin_left.unit,
                 (int)U::Px);
  test::ExpectEq("indent-left value", out["indent-left"].margin_left.value, 24);
  test::ExpectEq("indent-right unit",
                 (int)out["indent-right"].margin_right.unit, (int)U::Percent);
  test::ExpectEq("indent-right value", out["indent-right"].margin_right.value,
                 10);
  test::ExpectEq("indent-both left value", out["indent-both"].margin_left.value,
                 8);
  test::ExpectEq("indent-both right value",
                 out["indent-both"].margin_right.value, 8);
}

void TestLookupMarginLeftRightForClassAttr() {
  using U = book_xml_css_style_utils::MarginTopResult::Unit;
  CssClassMap map;
  map["left"].margin_left.unit = U::Px;
  map["left"].margin_left.value = 32;
  map["right"].margin_right.unit = U::Percent;
  map["right"].margin_right.value = 20;

  CssClassMargins merged;
  const bool found = epub_css_class_map::LookupMarginsForClassAttr(
      "left right", map, &merged);
  test::ExpectTrue("LookupMarginsForClassAttr finds left+right", found);
  test::ExpectEq("merged margin-left unit", (int)merged.margin_left.unit,
                 (int)U::Px);
  test::ExpectEq("merged margin-left value", merged.margin_left.value, 32);
  test::ExpectEq("merged margin-right unit", (int)merged.margin_right.unit,
                 (int)U::Percent);
  test::ExpectEq("merged margin-right value", merged.margin_right.value, 20);

  const auto ml =
      epub_css_class_map::LookupMarginLeftForClassAttr("left", map);
  test::ExpectEq("LookupMarginLeft unit", (int)ml.unit, (int)U::Px);
  test::ExpectEq("LookupMarginLeft value", ml.value, 32);

  const auto mr =
      epub_css_class_map::LookupMarginRightForClassAttr("right", map);
  test::ExpectEq("LookupMarginRight unit", (int)mr.unit, (int)U::Percent);
  test::ExpectEq("LookupMarginRight value", mr.value, 20);

  const auto miss =
      epub_css_class_map::LookupMarginLeftForClassAttr("right", map);
  test::ExpectEq("LookupMarginLeft miss -> None", (int)miss.unit,
                 (int)U::None);
}

void TestLookupTextIndentAndTransformFunctions() {
  using book_xml_css_style_utils::MarginTopResult;
  using book_xml_css_style_utils::TextTransform;

  CssClassMap map;
  map["ind"].text_indent.unit = MarginTopResult::Unit::Px;
  map["ind"].text_indent.value = 16;
  map["up"].has_text_transform = true;
  map["up"].text_transform = TextTransform::Uppercase;

  const MarginTopResult ti =
      epub_css_class_map::LookupTextIndentForClassAttr("ind", map);
  test::ExpectEq("LookupTextIndent unit", (int)ti.unit,
                 (int)MarginTopResult::Unit::Px);
  test::ExpectEq("LookupTextIndent value", ti.value, 16);

  const MarginTopResult ti_miss =
      epub_css_class_map::LookupTextIndentForClassAttr("up", map);
  test::ExpectEq("LookupTextIndent miss -> None", (int)ti_miss.unit,
                 (int)MarginTopResult::Unit::None);

  TextTransform tt = TextTransform::None;
  const bool found =
      epub_css_class_map::LookupTextTransformForClassAttr("up", map, &tt);
  test::ExpectTrue("LookupTextTransform finds class", found);
  test::ExpectEq("LookupTextTransform value", (int)tt, (int)TextTransform::Uppercase);

  TextTransform tt2 = TextTransform::None;
  const bool found2 =
      epub_css_class_map::LookupTextTransformForClassAttr("ind", map, &tt2);
  test::ExpectFalse("LookupTextTransform misses no-transform class", found2);
}

void TestParseCssIntoClassMapHandlesElementSelectors() {
  const char *css =
      "p { margin-top: 8px; margin-bottom: 12px; text-align: justify; }\n"
      "body { margin-left: 4px; }\n"
      "li { list-style-type: none; }\n"
      "h1 { font-size: 24px; }\n"
      "blockquote { margin-left: 16px; margin-right: 16px; }\n"
      ".override { text-align: center; }\n"
      "span { color: red; }\n";  // color unsupported → no entry

  CssClassMap out;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &out);

  using U = book_xml_css_style_utils::MarginTopResult::Unit;

  test::ExpectTrue("p element parsed",
                   out.find(std::string("*p")) != out.end());
  test::ExpectTrue("body element parsed",
                   out.find(std::string("*body")) != out.end());
  test::ExpectTrue("li element parsed",
                   out.find(std::string("*li")) != out.end());
  test::ExpectTrue("h1 element parsed",
                   out.find(std::string("*h1")) != out.end());
  test::ExpectTrue("blockquote element parsed",
                   out.find(std::string("*blockquote")) != out.end());
  test::ExpectTrue("class .override still parsed",
                   out.find("override") != out.end());
  test::ExpectTrue("span not parsed (no relevant props)",
                   out.find(std::string("*span")) == out.end());

  // p margin-top
  const CssClassMargins &p_css = out[std::string("*p")];
  test::ExpectEq("p margin-top unit", (int)p_css.margin_top.unit, (int)U::Px);
  test::ExpectEq("p margin-top value", p_css.margin_top.value, 8);
  // p margin-bottom
  test::ExpectEq("p margin-bottom unit", (int)p_css.margin_bottom.unit, (int)U::Px);
  test::ExpectEq("p margin-bottom value", p_css.margin_bottom.value, 12);
  // p text-align
  test::ExpectTrue("p has_text_align", p_css.has_text_align);
  test::ExpectEq("p text-align justify",
                 (int)p_css.text_align,
                 (int)book_xml_css_style_utils::TextAlign::Justify);
  // li list-style-type: none
  test::ExpectTrue("li hides list markers", out[std::string("*li")].hide_list_markers);
  // h1 font-size
  test::ExpectEq("h1 font-size unit",
                 (int)out[std::string("*h1")].font_size.unit,
                 (int)book_xml_css_style_utils::FontSizeSpec::Unit::Px);
  test::ExpectEq("h1 font-size value", out[std::string("*h1")].font_size.value_x100, 2400);
}

void TestLookupAllForTagAndMergeClassRulesToStyle() {
  using U = book_xml_css_style_utils::MarginTopResult::Unit;

  CssClassMap map;
  // p element rule: margin-top 8px, text-align justify
  map[std::string("*p")].margin_top.unit = U::Px;
  map[std::string("*p")].margin_top.value = 8;
  map[std::string("*p")].has_text_align = true;
  map[std::string("*p")].text_align = book_xml_css_style_utils::TextAlign::Left;
  // .center class rule: text-align center
  map["center"].has_text_align = true;
  map["center"].text_align = book_xml_css_style_utils::TextAlign::Center;

  // LookupAllForTag: fills from element rule
  CssClassMargins out;
  bool found = epub_css_class_map::LookupAllForTag("p", map, &out);
  test::ExpectTrue("LookupAllForTag finds p rule", found);
  test::ExpectEq("tag margin-top unit", (int)out.margin_top.unit, (int)U::Px);
  test::ExpectEq("tag margin-top value", out.margin_top.value, 8);
  test::ExpectTrue("tag has_text_align", out.has_text_align);

  // MergeClassRulesToStyle: overlay class rule on top → class wins text-align
  epub_css_class_map::MergeClassRulesToStyle("center", map, &out);
  test::ExpectTrue("merged has_text_align", out.has_text_align);
  test::ExpectEq("class overrides element text-align",
                 (int)out.text_align,
                 (int)book_xml_css_style_utils::TextAlign::Center);
  // Element margin-top survives merge
  test::ExpectEq("element margin-top survives", out.margin_top.value, 8);

  // LookupAllForTag for unknown tag returns false and resets
  CssClassMargins empty_out;
  bool not_found = epub_css_class_map::LookupAllForTag("div", map, &empty_out);
  test::ExpectFalse("LookupAllForTag unknown tag returns false", not_found);

  // LookupTextAlignForTag
  book_xml_css_style_utils::TextAlign align = book_xml_css_style_utils::TextAlign::Right;
  bool ta_found = epub_css_class_map::LookupTextAlignForTag("p", map, &align);
  test::ExpectTrue("LookupTextAlignForTag finds p", ta_found);
  test::ExpectEq("LookupTextAlignForTag value",
                 (int)align, (int)book_xml_css_style_utils::TextAlign::Left);
  // (map["*p"].text_align was set to Left directly)

  bool ta_miss = epub_css_class_map::LookupTextAlignForTag("div", map, &align);
  test::ExpectFalse("LookupTextAlignForTag misses unknown", ta_miss);
}

void TestElementSelectorCascadesOverTag() {
  // Verify: global p {} margin is applied, class .note overrides text-align
  const char *css =
      "p { margin-bottom: 10px; text-align: justify; }\n"
      ".note { text-align: center; margin-bottom: 20px; }\n";

  CssClassMap map;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &map);

  using U = book_xml_css_style_utils::MarginTopResult::Unit;

  // No class: tag rule applies
  CssClassMargins result;
  epub_css_class_map::LookupAllForTag("p", map, &result);
  epub_css_class_map::MergeClassRulesToStyle("", map, &result);
  test::ExpectEq("no-class margin-bottom from tag", result.margin_bottom.value, 10);
  test::ExpectEq("no-class text-align from tag",
                 (int)result.text_align,
                 (int)book_xml_css_style_utils::TextAlign::Justify);

  // With .note class: class overrides both margin-bottom and text-align
  CssClassMargins result2;
  epub_css_class_map::LookupAllForTag("p", map, &result2);
  epub_css_class_map::MergeClassRulesToStyle("note", map, &result2);
  test::ExpectEq("class margin-bottom wins", result2.margin_bottom.value, 20);
  test::ExpectEq("class text-align wins",
                 (int)result2.text_align,
                 (int)book_xml_css_style_utils::TextAlign::Center);
}

void TestParaCenterCssParseAndLookup() {
  // Exact CSS from the bug report.
  const char *css =
      ".paraCenter {\n"
      "    display: block;\n"
      "    text-align: center;\n"
      "    margin-top: 0%;\n"
      "    margin-bottom: 0%;\n"
      "}\n";

  CssClassMap map;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &map);

  // Entry must exist.
  test::ExpectTrue("paraCenter in map", map.count("paraCenter") > 0);

  const CssClassMargins &entry = map.at("paraCenter");

  // text-align: center must be captured.
  test::ExpectTrue("paraCenter has_text_align", entry.has_text_align);
  test::ExpectEq("paraCenter text_align is center",
                 (int)entry.text_align,
                 (int)book_xml_css_style_utils::TextAlign::Center);

  // display: block must be captured.
  test::ExpectTrue("paraCenter is_display_block", entry.is_display_block);

  // margin-top: 0% must be captured (unit=Percent, value=0).
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  test::ExpectEq("paraCenter margin-top unit is percent",
                 (int)entry.margin_top.unit, (int)Unit::Percent);
  test::ExpectEq("paraCenter margin-top value is 0", entry.margin_top.value, 0);

  // LookupTextAlignForClassAttr must return center.
  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  bool found =
      epub_css_class_map::LookupTextAlignForClassAttr("paraCenter", map, &align);
  test::ExpectTrue("LookupTextAlignForClassAttr finds paraCenter", found);
  test::ExpectEq("paraCenter align is center",
                 (int)align, (int)book_xml_css_style_utils::TextAlign::Center);

  // Multiple classes: "foo paraCenter" must also find center.
  book_xml_css_style_utils::TextAlign align2 =
      book_xml_css_style_utils::TextAlign::Left;
  bool found2 =
      epub_css_class_map::LookupTextAlignForClassAttr("foo paraCenter", map, &align2);
  test::ExpectTrue("LookupTextAlignForClassAttr finds paraCenter in multi-class",
                   found2);
  test::ExpectEq("multi-class paraCenter align is center",
                 (int)align2, (int)book_xml_css_style_utils::TextAlign::Center);
}

void TestLookupHideListMarkersForTag() {
  const char *css =
      "ol { list-style-type: none; }\n"
      "ul { list-style: none; margin: 0; }\n"
      "li { color: red; }\n"; // no list-style-type:none on li

  CssClassMap map;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &map);

  test::ExpectTrue("ol element rule hides markers",
                   epub_css_class_map::LookupHideListMarkersForTag("ol", map));
  test::ExpectTrue("ul element rule hides markers",
                   epub_css_class_map::LookupHideListMarkersForTag("ul", map));
  test::ExpectFalse("li without list-style-type:none does not hide",
                    epub_css_class_map::LookupHideListMarkersForTag("li", map));
  test::ExpectFalse("p not in map returns false",
                    epub_css_class_map::LookupHideListMarkersForTag("p", map));
}

void TestParseCssIntoClassMapDetectsDisplayNone() {
  const char *css =
      ".hide1 { display:none; }\n"
      ".shown { display: block; }\n";

  CssClassMap map;
  epub_css_class_map::ParseCssIntoClassMap(css, std::strlen(css), &map);

  test::ExpectTrue("hide1 class captured", map.find("hide1") != map.end());
  test::ExpectTrue("hide1 display none", map["hide1"].is_display_none);
  test::ExpectFalse("shown is not display none", map["shown"].is_display_none);

  CssClassMargins merged;
  test::ExpectTrue("display none merged from class attr",
                   epub_css_class_map::LookupAllForClassAttr(
                       "nav hide1", map, &merged, true));
  test::ExpectTrue("merged display none", merged.is_display_none);
}

} // namespace

int main() {
  TestParseCssIntoClassMapSupportsQualifiedAndGroupedSelectors();
  TestLookupMarginsForClassAttrMergesKnownClasses();
  TestLookupMarginsForClassAttrRejectsUnknownClasses();
  TestParseCssIntoClassMapDetectsListStyleNone();
  TestLookupTextAlignForClassAttrUsesLastKnownMatch();
  TestParseCssIntoClassMapDetectsTextAlignStartEnd();
  TestParseCssIntoClassMapDetectsWhiteSpaceModes();
  TestParseCssIntoClassMapDetectsPageBreakInsideAvoid();
  TestParseCssIntoClassMapDetectsPageBreakBeforeAfter();
  TestParseCssIntoClassMapDetectsFloatAndClear();
  TestLookupFontSizeForClassAttrUsesLastKnownMatch();
  TestParseCssIntoClassMapDetectsSuperSubScript();
  TestLookupSuperSubForClassAttr();
  TestParseCssIntoClassMapDetectsResetFlags();
  TestLookupResetFlagFunctions();
  TestParseCssIntoClassMapDetectsTextIndentAndTransform();
  TestLookupTextIndentAndTransformFunctions();
  TestParseCssIntoClassMapDetectsMarginLeftRight();
  TestLookupMarginLeftRightForClassAttr();
  TestParseCssIntoClassMapHandlesElementSelectors();
  TestLookupAllForTagAndMergeClassRulesToStyle();
  TestElementSelectorCascadesOverTag();
  TestParaCenterCssParseAndLookup();
  TestLookupHideListMarkersForTag();
  TestParseCssIntoClassMapDetectsDisplayNone();
  return 0;
}
