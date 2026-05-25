#include "book/book.h"
#include "book/book_context.h"
#include "book/book_xml.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_screen_advance.h"
#include "book/page.h"
#include "formats/common/xml_parse_utils.h"
#include "parse.h"
#include "shared/text_token_constants.h"
#include "ui/text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectTrue(const char *label, bool v) {
  if (!v) Fail(label, "expected true");
  g_pass++;
}

void ExpectFalse(const char *label, bool v) {
  if (v) Fail(label, "expected false");
  g_pass++;
}

bool BufContains(const u32 *buf, int len, u32 value) {
  for (int i = 0; i < len; i++)
    if (buf[i] == value)
      return true;
  return false;
}

int CountBufValue(const u32 *buf, int len, u32 value) {
  int count = 0;
  for (int i = 0; i < len; i++)
    if (buf[i] == value)
      count++;
  return count;
}

struct TestCtx {
  Text text;
  BookContext ctx;
  unsigned char paragraph_spacing;
  unsigned char paragraph_indent;
  bool publisher_text_indent;
  bool publisher_block_margins;

  TestCtx() {
    ctx.text = &text;
    ctx.prefs = nullptr;
    ctx.status_reporter = nullptr;
    paragraph_spacing = 1;
    paragraph_indent = 0;
    publisher_text_indent = true;
    publisher_block_margins = true;
    ctx.paragraph_spacing = &paragraph_spacing;
    ctx.paragraph_indent = &paragraph_indent;
    ctx.publisher_text_indent = &publisher_text_indent;
    ctx.publisher_block_margins = &publisher_block_margins;
    ctx.orientation = nullptr;
    ctx.draw_background = nullptr;
    ctx.draw_background_user_data = nullptr;
    ctx.draw_top_background = nullptr;
    ctx.draw_top_background_user_data = nullptr;
    ctx.on_spine_progress = nullptr;
    ctx.on_spine_progress_user_data = nullptr;
  }
};

parsedata_t MakeParseData(TestCtx &tc, Book &book) {
  parsedata_t p{};
  parse_init(&p);
  p.ts = tc.ctx.text;
  p.book = &book;
  p.base_font_size_px = 14;
  p.pen.y = 10;
  return p;
}

xml_parse_utils::XmlParserOptions MakeXmlOpts(parsedata_t *p) {
  xml_parse_utils::XmlParserOptions opts;
  opts.start_element = xml::book::start;
  opts.end_element = xml::book::end;
  opts.character_data = xml::book::chardata;
  opts.user_data = p;
  return opts;
}

// </body> flushes p.buf into a new Page via book.AppendPage().
// Inspect that page's buffer for the presence/absence of tokens.

void TestRubyAnnotationEmitsBrackets() {
  // <rt> handler emits '(' before annotation text and ')' after, at 75% size.
  // Verify that both parens and a TEXT_FONT_SIZE token appear in page output.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body><ruby>Base<rt>rt</rt></ruby></body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("ruby: parse ok", r.ok);
  ExpectTrue("ruby: page produced", book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  ExpectTrue("ruby: open paren in output", BufContains(buf, len, '('));
  ExpectTrue("ruby: close paren in output", BufContains(buf, len, ')'));
  ExpectTrue("ruby: font-size token emitted", BufContains(buf, len, TEXT_FONT_SIZE));
}

void TestTableImgSuppressed() {
  // Before the fix, <img> inside <td> appended "[image]" to the cell text
  // buffer, which then appeared as literal text in the page output. After the
  // fix, table cell images are silently suppressed.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body>"
      "<table><tr><td><img src=\"cover.png\"/></td></tr></table>"
      "</body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("table-img: parse ok", r.ok);

  if (book.GetPageCount() > 0) {
    const u32 *buf = book.GetPage(0)->GetBuffer();
    const int len = book.GetPage(0)->GetLength();
    ExpectFalse("table-img: no '[' in page output", BufContains(buf, len, '['));
  }
  // Also check any residual in p.buf (should be empty after </body> flush).
  ExpectFalse("table-img: no '[' in residual buf", BufContains(p.buf, p.buflen, '['));
}

void TestHiddenElementsDoNotEmitLayoutTokens() {
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body>"
      "<div style=\"display:block\">"
      "<h1 class=\"visually-hidden\">QZX</h1>"
      "<p style=\"display:none\">QZX</p>"
      "<div style=\"position:absolute;width:1px;height:1px;"
      "clip-path:inset(100%);overflow:hidden\">QZX</div>"
      "</div>"
      "<p>Visible description</p>"
      "</body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("hidden-elements: parse ok", r.ok);
  ExpectTrue("hidden-elements: page produced", book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  ExpectFalse("hidden-elements: no hidden Q emitted", BufContains(buf, len, 'Q'));
  ExpectFalse("hidden-elements: no hidden Z emitted", BufContains(buf, len, 'Z'));
  ExpectFalse("hidden-elements: no hidden X emitted", BufContains(buf, len, 'X'));
  ExpectFalse("hidden-elements: no heading bold emitted",
              BufContains(buf, len, TEXT_BOLD_ON));
  ExpectFalse("hidden-elements: no heading font-size emitted",
              BufContains(buf, len, TEXT_FONT_SIZE));
  ExpectTrue("hidden-elements: visible text remains", BufContains(buf, len, 'V'));
}

void TestUserParagraphSpacingAddsExtraBlankLines() {
  TestCtx tc;
  tc.paragraph_spacing = 2;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body><p>First paragraph.</p><p>Second paragraph.</p></body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("paragraph-spacing: parse ok", r.ok);
  ExpectTrue("paragraph-spacing: page produced", book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  ExpectTrue("paragraph-spacing: extra blank lines emitted",
             CountBufValue(buf, len, '\n') >= 3);
}

void TestBlockIndentSurvivesPageOverflow() {
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  p.screen = 1;
  p.pen.y = 390;
  p.pen.x = tc.text.margin.left + 80;
  p.current_screen_has_drawable_content = true;
  parse_set_current_block_margins(&p, 36, 0);
  parse_append_page_byte(&p, 'x');
  p.linebegan = true;

  FlowEmissionFns fns{};
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.flush_pending_block = [](parsedata_t *pd, const char *tag) {
    book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(pd, tag);
  };

  book_xml_flow_emission::EmitFlowedFragmentRaw(
      &p, "Indented continuation line", 26, fns);

  ExpectTrue("block-overflow: previous page produced", book.GetPageCount() > 0);
  ExpectTrue("block-overflow: residual buffer contains line-start marker",
             p.buflen >= 2 && p.buf[0] == TEXT_LINE_START_X);
  ExpectTrue("block-overflow: new page starts at block margin",
             p.buflen >= 2 && p.buf[1] > (u32)tc.text.margin.left);
}

} // namespace

int main() {
  TestRubyAnnotationEmitsBrackets();
  TestTableImgSuppressed();
  TestHiddenElementsDoNotEmitLayoutTokens();
  TestUserParagraphSpacingAddsExtraBlankLines();
  TestBlockIndentSurvivesPageOverflow();
  printf("PASS: %d tests\n", g_pass);
  return 0;
}
