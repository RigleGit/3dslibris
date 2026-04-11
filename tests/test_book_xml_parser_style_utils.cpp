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

} // namespace

int main() {
  TestResolveParsedTextStylePrefersMono();
  TestRestoreParsedStyleMarkersReinjectsMono();
  TestRestoreParsedStyleMarkersReinjectsPreContext();
  return 0;
}
