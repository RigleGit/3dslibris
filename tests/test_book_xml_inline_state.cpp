#include "book/book_xml_inline_state.h"
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

void TestHasActiveStackBoldStyle() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectFalse("empty stack: no bold", book_xml_inline_state::HasActiveStackBoldStyle(&p));
  parse_push(&p, TAG_STRONG);
  p.style_bold_stack[p.stacksize - 1] = true;
  test::ExpectTrue("bold set at top", book_xml_inline_state::HasActiveStackBoldStyle(&p));
  test::ExpectFalse("null returns false", book_xml_inline_state::HasActiveStackBoldStyle(nullptr));
}

void TestHasActiveStackItalicStyle() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectFalse("no italic", book_xml_inline_state::HasActiveStackItalicStyle(&p));
  parse_push(&p, TAG_STRONG);
  p.style_italic_stack[p.stacksize - 1] = true;
  test::ExpectTrue("italic set", book_xml_inline_state::HasActiveStackItalicStyle(&p));
}

void TestHasActiveStackUnderlineStyle() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectFalse("no underline", book_xml_inline_state::HasActiveStackUnderlineStyle(&p));
  parse_push(&p, TAG_STRONG);
  p.style_underline_stack[p.stacksize - 1] = true;
  test::ExpectTrue("underline set", book_xml_inline_state::HasActiveStackUnderlineStyle(&p));
}

void TestHasActiveStackMonoStyle() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectFalse("no mono", book_xml_inline_state::HasActiveStackMonoStyle(&p));
  parse_push(&p, TAG_STRONG);
  p.style_mono_stack[p.stacksize - 1] = true;
  test::ExpectTrue("mono set", book_xml_inline_state::HasActiveStackMonoStyle(&p));
}

void TestHasActiveStackHiddenStyle() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectFalse("no hidden", book_xml_inline_state::HasActiveStackHiddenStyle(&p));
  parse_push(&p, TAG_STRONG);
  p.style_hidden_stack[p.stacksize - 1] = true;
  test::ExpectTrue("hidden set", book_xml_inline_state::HasActiveStackHiddenStyle(&p));
}

void TestHasActiveStackDeepStack() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_STRONG);
  parse_push(&p, TAG_STRONG);
  parse_push(&p, TAG_STRONG);
  p.style_bold_stack[0] = true;
  test::ExpectTrue("bold in first level found", book_xml_inline_state::HasActiveStackBoldStyle(&p));
  p.style_bold_stack[0] = false;
  p.style_bold_stack[2] = true;
  test::ExpectTrue("bold in top level found", book_xml_inline_state::HasActiveStackBoldStyle(&p));
}

void TestResolveActiveUnderlineStyleFromStyleStack() {
  parsedata_t p{};
  parse_init(&p);
  test::ExpectEq("default underline", (int)book_xml_inline_state::ResolveActiveUnderlineStyle(&p), UNDERLINE_STYLE_SOLID);
  test::ExpectEq("null returns solid", (int)book_xml_inline_state::ResolveActiveUnderlineStyle(nullptr), UNDERLINE_STYLE_SOLID);
  parse_push(&p, TAG_STRONG);
  p.style_underline_stack[p.stacksize - 1] = true;
  p.style_underline_style_stack[p.stacksize - 1] = UNDERLINE_STYLE_WAVY;
  test::ExpectEq("wavy from style stack", (int)book_xml_inline_state::ResolveActiveUnderlineStyle(&p), UNDERLINE_STYLE_WAVY);
}

void TestResolveActiveUnderlineStyleFromTagStack() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_UNDERLINE);
  test::ExpectEq("solid from TAG_UNDERLINE", (int)book_xml_inline_state::ResolveActiveUnderlineStyle(&p), UNDERLINE_STYLE_SOLID);
}

void TestResolveActiveUnderlineStylePrefersStyleStack() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_UNDERLINE);
  parse_push(&p, TAG_STRONG);
  p.style_underline_stack[p.stacksize - 1] = true;
  p.style_underline_style_stack[p.stacksize - 1] = UNDERLINE_STYLE_WAVY;
  test::ExpectEq("style stack wins over tag", (int)book_xml_inline_state::ResolveActiveUnderlineStyle(&p), UNDERLINE_STYLE_WAVY);
}

void TestGetTopActiveInlineLink() {
  parsedata_t p{};
  parse_init(&p);
  u16 href = 0;
  test::ExpectFalse("no link in empty stack", book_xml_inline_state::GetTopActiveInlineLink(&p, &href));
  parse_push(&p, TAG_ANCHOR);
  p.link_active_stack[p.stacksize - 1] = true;
  p.link_href_id_stack[p.stacksize - 1] = 42;
  test::ExpectTrue("link found", book_xml_inline_state::GetTopActiveInlineLink(&p, &href));
  test::ExpectEq("href id", (int)href, 42);
}

void TestGetTopActiveInlineLinkSkipsZeroId() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_ANCHOR);
  p.link_active_stack[p.stacksize - 1] = true;
  p.link_href_id_stack[p.stacksize - 1] = 0;
  u16 href = 99;
  test::ExpectFalse("zero id skipped", book_xml_inline_state::GetTopActiveInlineLink(&p, &href));
}

void TestGetTopActiveInlineLinkTopmost() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_ANCHOR);
  p.link_active_stack[0] = true;
  p.link_href_id_stack[0] = 10;
  parse_push(&p, TAG_ANCHOR);
  p.link_active_stack[1] = true;
  p.link_href_id_stack[1] = 20;
  u16 href = 0;
  book_xml_inline_state::GetTopActiveInlineLink(&p, &href);
  test::ExpectEq("topmost link returned", (int)href, 20);
}

void TestRestoreParsedInlineLinkMarkerNoLink() {
  parsedata_t p{};
  parse_init(&p);
  book_xml_inline_state::RestoreParsedInlineLinkMarker(&p);
  test::ExpectEq("no bytes emitted without active link", p.buflen, 0);
}

void TestRestoreParsedInlineLinkMarkerEmitsTokens() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_ANCHOR);
  p.link_active_stack[p.stacksize - 1] = true;
  p.link_href_id_stack[p.stacksize - 1] = 7;
  book_xml_inline_state::RestoreParsedInlineLinkMarker(&p);
  test::ExpectEq("two bytes emitted", p.buflen, 2);
  test::ExpectEq("TEXT_LINK_START token", (int)p.buf[0], TEXT_LINK_START);
  test::ExpectEq("href id value", (int)p.buf[1], 7);
}

void TestQueueDeferredStyleSync() {
  parsedata_t p{};
  parse_init(&p);
  book_xml_inline_state::QueueDeferredStyleSync(&p, true, false, true,
                                                UNDERLINE_STYLE_WAVY, false,
                                                false, false, false, true);
  test::ExpectTrue("deferred_style_sync set", p.deferred_style_sync);
  test::ExpectTrue("target bold", p.deferred_target_bold);
  test::ExpectFalse("target italic", p.deferred_target_italic);
  test::ExpectTrue("target underline", p.deferred_target_underline);
  test::ExpectEq("target underline style", (int)p.deferred_target_underline_style, UNDERLINE_STYLE_WAVY);
  test::ExpectTrue("target mono", p.deferred_target_mono);
}

void TestQueueDeferredStyleSyncNull() {
  book_xml_inline_state::QueueDeferredStyleSync(nullptr, true, true, true,
                                                UNDERLINE_STYLE_SOLID, false,
                                                false, false, false, false);
}

} // namespace

int main() {
  TestHasActiveStackBoldStyle();
  TestHasActiveStackItalicStyle();
  TestHasActiveStackUnderlineStyle();
  TestHasActiveStackMonoStyle();
  TestHasActiveStackHiddenStyle();
  TestHasActiveStackDeepStack();
  TestResolveActiveUnderlineStyleFromStyleStack();
  TestResolveActiveUnderlineStyleFromTagStack();
  TestResolveActiveUnderlineStylePrefersStyleStack();
  TestGetTopActiveInlineLink();
  TestGetTopActiveInlineLinkSkipsZeroId();
  TestGetTopActiveInlineLinkTopmost();
  TestRestoreParsedInlineLinkMarkerNoLink();
  TestRestoreParsedInlineLinkMarkerEmitsTokens();
  TestQueueDeferredStyleSync();
  TestQueueDeferredStyleSyncNull();
  return 0;
}
