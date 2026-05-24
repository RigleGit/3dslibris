#include "book/book_xml_parser_style_utils.h"
#include "parse.h"
#include "shared/text_token_constants.h"
#include "test_assert.h"

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

void TestResolveParsedTextStylePrefersMono() {
  test::ExpectEq("regular style",
                 book_xml_parser_style_utils::ResolveParsedTextStyle(
                     false, false, false),
                 TEXT_STYLE_REGULAR);
  test::ExpectEq("bold italic style",
                 book_xml_parser_style_utils::ResolveParsedTextStyle(
                     true, true, false),
                 TEXT_STYLE_BOLDITALIC);
  test::ExpectEq("mono overrides bold italic",
                 book_xml_parser_style_utils::ResolveParsedTextStyle(
                     true, true, true),
                 TEXT_STYLE_MONO_BOLDITALIC);
  test::ExpectEq("mono overrides italic",
                 book_xml_parser_style_utils::ResolveParsedTextStyle(
                     false, true, true),
                 TEXT_STYLE_MONO_ITALIC);
  test::ExpectEq("mono bold style",
                 book_xml_parser_style_utils::ResolveParsedTextStyle(
                     true, false, true),
                 TEXT_STYLE_MONO_BOLD);
}

void TestRestoreParsedStyleMarkersReinjectsMono() {
  parsedata_t p{};
  parse_init(&p);
  p.italic = true;
  p.bold = true;
  p.overline = true;
  p.underline = true;
  p.underline_style = UNDERLINE_STYLE_WAVY;
  p.mono = true;

  book_xml_parser_style_utils::RestoreParsedStyleMarkers(&p);

  test::ExpectEq("marker count", p.buflen, 7);
  test::ExpectEq("overline marker", (int)p.buf[0], TEXT_OVERLINE_ON);
  test::ExpectEq("underline marker", (int)p.buf[1], TEXT_UNDERLINE_ON);
  test::ExpectEq("underline style token", (int)p.buf[2], TEXT_UNDERLINE_STYLE);
  test::ExpectEq("underline style value", (int)p.buf[3],
                 UNDERLINE_STYLE_WAVY);
  test::ExpectEq("italic marker", (int)p.buf[4], TEXT_ITALIC_ON);
  test::ExpectEq("bold marker", (int)p.buf[5], TEXT_BOLD_ON);
  test::ExpectEq("mono marker", (int)p.buf[6], TEXT_MONO_ON);
}

void TestRestoreParsedStyleMarkersReinjectsPreContext() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_PRE);
  p.mono = true;

  book_xml_parser_style_utils::RestoreParsedStyleMarkers(&p);

  test::ExpectEq("marker count", p.buflen, 2);
  test::ExpectEq("pre marker", (int)p.buf[0], TEXT_PRE_ON);
  test::ExpectEq("mono marker", (int)p.buf[1], TEXT_MONO_ON);
}

void TestRestoreParsedStyleMarkersKeepsParagraphAlignmentContext() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_P);
  p.last_p_style = "text-align: right";

  book_xml_parser_style_utils::RestoreParsedStyleMarkers(&p);

  test::ExpectEq("paragraph alignment marker count", p.buflen, 1);
  test::ExpectEq("paragraph right marker", (int)p.buf[0], TEXT_PARAGRAPH_RIGHT);
}

void TestRestoreParsedStyleMarkersKeepsBlockAlignmentContext() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_DIV);
  p.block_text_align_stack[0] = true;
  p.block_text_align_value_stack[0] =
      (u8)book_xml_css_style_utils::TextAlign::Center;

  book_xml_parser_style_utils::RestoreParsedStyleMarkers(&p);

  test::ExpectEq("block alignment marker count", p.buflen, 1);
  test::ExpectEq("block center marker", (int)p.buf[0], TEXT_PARAGRAPH_CENTER);
}

void TestRestoreParsedStyleMarkersInheritsBlockAlignmentInParagraph() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_DIV);
  p.block_text_align_stack[0] = true;
  p.block_text_align_value_stack[0] =
      (u8)book_xml_css_style_utils::TextAlign::Center;
  parse_push(&p, TAG_P);

  book_xml_parser_style_utils::RestoreParsedStyleMarkers(&p);

  test::ExpectEq("paragraph inherits block marker count", p.buflen, 1);
  test::ExpectEq("paragraph inherited center marker", (int)p.buf[0],
                 TEXT_PARAGRAPH_CENTER);
}

void TestResolveCssMarginLinefeedsUsesCeilQuantization() {
  book_xml_css_style_utils::MarginTopResult m{};
  m.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  m.value = 24;
  test::ExpectEq("24px at 16px line height -> 2 linefeeds",
                 book_xml_parser_style_utils::ResolveCssMarginLinefeeds(m, 16),
                 2);

  m.unit = book_xml_css_style_utils::MarginTopResult::Unit::Percent;
  m.value = 10;
  test::ExpectEq("10 percent of 240 at 20px line height -> 2 linefeeds",
                 book_xml_parser_style_utils::ResolveCssMarginLinefeeds(m, 20),
                 2);
}

void TestResolveBlockSpacingLinefeedsHonorsDefaultsAndZero() {
  book_xml_css_style_utils::MarginTopResult none{};
  test::ExpectEq("none keeps top default",
                 book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
                     1, none, 16),
                 1);
  test::ExpectEq("none keeps bottom default",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, none, 16),
                 2);

  book_xml_css_style_utils::MarginTopResult zero{};
  zero.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  zero.value = 0;
  test::ExpectEq("zero suppresses top default",
                 book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
                     1, zero, 16),
                 0);
  test::ExpectEq("zero suppresses bottom default",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, zero, 16),
                 0);

  book_xml_css_style_utils::MarginTopResult positive{};
  positive.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  positive.value = 24;
  test::ExpectEq("positive top keeps css target when already above default",
                 book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
                     1, positive, 16),
                 2);
  test::ExpectEq("positive bottom forces one extra line when css equals default",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, positive, 16),
                 3);
}

void TestResolveBlockSpacingLinefeedsKeepsSmallExplicitMarginsVisible() {
  book_xml_css_style_utils::MarginTopResult small_top{};
  small_top.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  small_top.value = 4;
  test::ExpectEq("small explicit top still adds visible spacing",
                 book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
                     1, small_top, 15),
                 2);

  book_xml_css_style_utils::MarginTopResult small_bottom{};
  small_bottom.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  small_bottom.value = 20;
  test::ExpectEq("small explicit bottom still adds visible spacing",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, small_bottom, 15),
                 3);

  book_xml_css_style_utils::MarginTopResult medium_top{};
  medium_top.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  medium_top.value = 48;
  test::ExpectEq("48px top resolves to css target instead of over-adding",
                 book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
                     1, medium_top, 15),
                 4);

  book_xml_css_style_utils::MarginTopResult large_bottom{};
  large_bottom.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  large_bottom.value = 64;
  test::ExpectEq("64px bottom resolves to css target instead of over-adding",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, large_bottom, 15),
                 4);

  book_xml_css_style_utils::MarginTopResult capped{};
  capped.unit = book_xml_css_style_utils::MarginTopResult::Unit::Px;
  capped.value = 96;
  test::ExpectEq("explicit margins are capped to avoid runaway spacing",
                 book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                     2, capped, 15),
                 4);
}

void TestZeroTopMarginDoesNotErasePendingParentCssSpacing() {
  test::ExpectFalse(
      "zero child top margin keeps pending parent css spacing",
      book_xml_parser_style_utils::ShouldZeroMarginSuppressPendingSpacing(
          "paragraph-top", true, false, 1));
  test::ExpectTrue(
      "zero top margin still suppresses non-css default spacing",
      book_xml_parser_style_utils::ShouldZeroMarginSuppressPendingSpacing(
          "paragraph-top", false, false, 1));
  test::ExpectTrue(
      "zero bottom margin still suppresses previous spacing",
      book_xml_parser_style_utils::ShouldZeroMarginSuppressPendingSpacing(
          "paragraph-bottom", true, false, 1));
}

void TestComputeHeadingFontSizeUsesDefaultMultipliers() {
  epub_css_class_map::CssClassMap classes;
  test::ExpectEq("h1 multiplier",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 1, "", "", classes),
                 21);
  test::ExpectEq("h2 multiplier",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 2, "", "", classes),
                 18);
  test::ExpectEq("h3 multiplier",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 3, "", "", classes),
                 16);
  test::ExpectEq("h4 stays at base",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 4, "", "", classes),
                 14);
}

void TestComputeHeadingFontSizeUsesCssAndClamps() {
  epub_css_class_map::CssClassMap classes;
  classes["hero"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Px;
  classes["hero"].font_size.value_x100 = 4000;
  classes["tiny"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Px;
  classes["tiny"].font_size.value_x100 = 600;

  test::ExpectEq("class font-size clamped high",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 1, "", "hero", classes),
                 28);
  test::ExpectEq("class font-size clamped low",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 2, "", "tiny", classes),
                 11);
  test::ExpectEq("inline font-size overrides class",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 3, "font-size: 19px;", "tiny", classes),
                 17);
}

void TestComputeHeadingFontSizeSupportsRelativeCssUnits() {
  epub_css_class_map::CssClassMap classes;
  classes["percent"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Percent;
  classes["percent"].font_size.value_x100 = 15000;
  classes["em"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Em;
  classes["em"].font_size.value_x100 = 140;
  classes["rem"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Rem;
  classes["rem"].font_size.value_x100 = 125;
  classes["smaller"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Smaller;
  classes["larger"].font_size.unit =
      book_xml_css_style_utils::FontSizeSpec::Unit::Larger;

  test::ExpectEq("class percent resolved",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 2, "", "percent", classes),
                 21);
  test::ExpectEq("class em resolved",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 2, "", "em", classes),
                 20);
  test::ExpectEq("class rem resolved",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 2, "", "rem", classes),
                 18);
  test::ExpectEq("class smaller resolved",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 4, "", "smaller", classes),
                 12);
  test::ExpectEq("inline larger resolved",
                 book_xml_parser_style_utils::ComputeHeadingFontSize(
                     14, 4, "font-size: larger;", "smaller", classes),
                 17);
}

void TestComputeHeadingFontSizeForContextUsesDocBase() {
  epub_css_class_map::CssClassMap classes;

  // Default h2: inherited == doc base (no nesting) — same as old behavior.
  test::ExpectEq("h2 default no nesting",
                 book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                     14, 14, 2, "", "", classes),
                 18);

  // Bug scenario: <small><h2> — inherited_px reduced to 11, doc base stays 14.
  // Default multiplier must apply to doc base (14), not the reduced context (11).
  test::ExpectEq("h2 default inside <small> uses doc base",
                 book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                     11, 14, 2, "", "", classes),
                 18);

  // Explicit CSS on heading: resolves against inherited_px (current context).
  // <small><h2 style="font-size:1.3em"> → 1.3×11 = 14.
  test::ExpectEq("h2 explicit CSS inside <small> uses inherited context",
                 book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                     11, 14, 2, "font-size: 1.3em;", "", classes),
                 14);

  // h1 default inside <small>.
  test::ExpectEq("h1 default inside <small> uses doc base",
                 book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                     11, 14, 1, "", "", classes),
                 21);

  // Regression test for Bug 2: h2 inside <small> at the typical 12px body
  // size.  <small> reduces inherited_px to 10 (12 / 1.2).  The heading font
  // size must be computed from doc_base=12, giving 16px (12 * 1.3 ≈ 16).  If
  // heading_px ever equals inherited_px (10), ApplyHeadingFontSize returns
  // early without emitting a font-size token, which silently drops the heading
  // visual and lets the mandatory block break logic run at the wrong size.
  test::ExpectEq("h2 inside small at 12px base gets heading size from doc base",
                 book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                     10, 12, 2, "", "", classes),
                 16);
  // Explicitly verify that the result differs from the inherited small size so
  // ApplyHeadingFontSize cannot take the early-return path.
  test::ExpectTrue("h2 heading size differs from inherited small size",
                   book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
                       10, 12, 2, "", "", classes) != 10);
}

void TestClampInlineFontSizeKeepsNestedSmallReadable() {
  test::ExpectEq("nested small clamped against reading base",
                 book_xml_parser_style_utils::ClampInlineFontSize(14, 9),
                 11);
  test::ExpectEq("normal small remains readable",
                 book_xml_parser_style_utils::ClampInlineFontSize(14, 11),
                 11);
  test::ExpectEq("inline large capped against reading base",
                 book_xml_parser_style_utils::ClampInlineFontSize(14, 40),
                 20);
}

} // namespace

int main() {
  TestResolveParsedTextStylePrefersMono();
  TestRestoreParsedStyleMarkersReinjectsMono();
  TestRestoreParsedStyleMarkersReinjectsPreContext();
  TestRestoreParsedStyleMarkersKeepsParagraphAlignmentContext();
  TestRestoreParsedStyleMarkersKeepsBlockAlignmentContext();
  TestRestoreParsedStyleMarkersInheritsBlockAlignmentInParagraph();
  TestResolveCssMarginLinefeedsUsesCeilQuantization();
  TestResolveBlockSpacingLinefeedsHonorsDefaultsAndZero();
  TestResolveBlockSpacingLinefeedsKeepsSmallExplicitMarginsVisible();
  TestZeroTopMarginDoesNotErasePendingParentCssSpacing();
  TestComputeHeadingFontSizeUsesDefaultMultipliers();
  TestComputeHeadingFontSizeUsesCssAndClamps();
  TestComputeHeadingFontSizeSupportsRelativeCssUnits();
  TestComputeHeadingFontSizeForContextUsesDocBase();
  TestClampInlineFontSizeKeepsNestedSmallReadable();
  return 0;
}
