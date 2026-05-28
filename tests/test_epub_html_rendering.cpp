#include "book/book.h"
#include "book/book_context.h"
#include "book/book_xml.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_screen_advance.h"
#include "book/page.h"
#include "formats/common/xml_parse_utils.h"
#include "parse.h"
#include "shared/text_token_constants.h"
#include "shared/screen_dimensions.h"
#include "shared/text_render_layout_utils.h"
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

void TestUserParagraphSpacingSurvivesZeroPublisherMargin() {
  TestCtx tc;
  tc.paragraph_spacing = 2;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><head><style>p{margin-bottom:0}</style></head><body>"
      "<p>First paragraph.</p><p>Second paragraph.</p></body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("paragraph-spacing-zero-margin: parse ok", r.ok);
  ExpectTrue("paragraph-spacing-zero-margin: page produced",
             book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  ExpectTrue("paragraph-spacing-zero-margin: user spacing survives CSS zero",
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

void TestFontSizeRestoreClearsSuppressOnlyFlag() {
  // Rule: pending_block_spacing_suppress_only set inside a font-size scope
  // (e.g. from a zero-margin image container inside a 200%-font <div>) must
  // NOT propagate past the scope boundary.  After restore_font_size_px fires
  // the flag should be false so the following block element does not
  // incorrectly inherit the suppress signal and emit an extra blank line.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  const u8 base_px = 14;
  const u8 inflated_px = 28;

  // Set font to 200% (inside the large-container div).
  tc.text.SetPixelSize(inflated_px);

  // Simulate the suppress_only flag being set by a zero-margin image paragraph
  // that closed while inside the font-size scope.
  p.pending_block_spacing_suppress_only = true;
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_lf = 0;

  // Push stack entry for the open <div style="font-size:200%">.
  parse_push(&p, TAG_DIV);
  p.style_font_size_restore_stack[(u8)(p.stacksize - 1)] = base_px;

  // Close the div — font restore fires and must clear suppress_only.
  xml::book::end(&p, "div");

  ExpectTrue("font-restore-clears-suppress: font restored",
             (int)tc.text.GetPixelSize() == (int)base_px);
  ExpectFalse("font-restore-clears-suppress: suppress_only cleared",
              p.pending_block_spacing_suppress_only);
}

void TestFontSizeRestoreAdjustsPenYAfterBlockImageOverflow() {
  // Rule: when a font-size element is closed (e.g. </div> with font-size:200%)
  // while pen.y sits at the top of a freshly advanced screen — meaning
  // advance_page_overflow fired during a block image inside the element — the
  // parser must correct pen.y from (margin.top + inflated_lh) down to
  // (margin.top + restored_lh).  Without the fix pen.y stays too high,
  // causing subsequent spacing decisions to underestimate available space.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  const u8 base_px = 14;
  const u8 inflated_px = 28; // 200% of 14
  const int top = tc.text.margin.top; // 10 (from Text stub)

  // Simulate the state left by advance_page_overflow during a BAND image
  // inside a font-size:200% container: font is inflated, pen.y lands at
  // margin.top + inflated lineheight, and the line has not started yet.
  tc.text.SetPixelSize(inflated_px);
  p.pen.y = top + (int)tc.text.GetHeight(); // 10 + 28 = 38
  p.linebegan = false;
  p.current_screen_has_drawable_content = false;

  // Push a stack entry representing the open <div style="font-size:200%">.
  // Record the pre-change pixel size so endElement knows what to restore.
  parse_push(&p, TAG_DIV);
  p.style_font_size_restore_stack[(u8)(p.stacksize - 1)] = base_px;

  // Fire the end element for </div>.  This triggers font-size restore and,
  // with the fix, the pen.y correction.
  xml::book::end(&p, "div");

  const int expected_pen_y = top + (int)tc.text.GetHeight(); // 10 + 14 = 24
  ExpectTrue("font-restore-pen-y: font restored to base_px",
             (int)tc.text.GetPixelSize() == (int)base_px);
  ExpectTrue("font-restore-pen-y: pen.y corrected to restored lineheight",
             p.pen.y == expected_pen_y);
}

void TestSuppressOnlyDoesNotCrossBlockFontScopeStart() {
  // Rule: pending_block_spacing_suppress_only set by a zero-margin block
  // must NOT propagate through a block-level font-size scope boundary into
  // the first paragraph inside, even when the declared font-size is clamped
  // to the same pixel value as the current font (kTextPixelSizeMax reached).
  TestCtx tc;
  tc.paragraph_spacing = 0;  // isolate publisher CSS margin behavior
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  // Set font to max so 200% is clamped to the same value, exercising the
  // has_spec path that fires even when new_font_px resolves to 0.
  p.ts->SetPixelSize(20);  // kTextPixelSizeMax
  p.base_font_size_px = 20;
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  // <p> with margin:0 followed by a font-size:200% div whose first <p> has
  // margin-top:0.5em. Without the fix the inner paragraph receives an extra
  // blank line from was_suppressed injection; with the fix there is none.
  // Using inline styles because the test does not populate a CSS class map.
  const std::string html =
      "<html><body>"
      "<p style=\"margin-top:0;margin-bottom:0\">placeholder</p>"
      "<div style=\"font-size:200%\">"
      "<p style=\"margin-top:0.5em\">Inner text</p>"
      "</div>"
      "</body></html>";
  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("suppress-font-scope: parse ok", r.ok);
  ExpectTrue("suppress-font-scope: page produced", book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  // Exactly one block boundary newline before "Inner text" (from
  // EnsureBlockBoundaryBeforeBlockStart). A second newline would indicate
  // the spurious blank line from was_suppressed injection.
  ExpectTrue("suppress-font-scope: no extra blank line before inner paragraph",
             CountBufValue(buf, len, '\n') == 1);
}

void TestCssSpacingNearBottomAdvancesScreen() {
  // Regression: when a pending block break comes from explicit CSS spacing
  // and only one line remains, keep paragraph rhythm by advancing to the
  // next screen instead of consuming the last line with a compressed break.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  p.buflen = 1;
  p.buf[0] = 'A';
  p.linebegan = true;
  p.current_screen_has_drawable_content = true;
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 0;
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = false;
  p.screen = 0;

  const int line_step = tc.text.GetHeight() + tc.text.linespacing;
  const int compact_bottom =
      text_render_layout_utils::ResolveCompactReadingBottomMargin(tc.text.margin.bottom);
  const int max_height = screen_dims::kTopScreenHeightPx;
  const int target_usable = line_step; // exactly one line available
  p.pen.y = max_height - compact_bottom - target_usable;

  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "p");

  ExpectTrue("css-spacing-bottom: advanced to next screen", p.screen == 1);
}

} // namespace

int main() {
  TestRubyAnnotationEmitsBrackets();
  TestTableImgSuppressed();
  TestHiddenElementsDoNotEmitLayoutTokens();
  TestUserParagraphSpacingAddsExtraBlankLines();
  TestUserParagraphSpacingSurvivesZeroPublisherMargin();
  TestBlockIndentSurvivesPageOverflow();
  TestFontSizeRestoreClearsSuppressOnlyFlag();
  TestFontSizeRestoreAdjustsPenYAfterBlockImageOverflow();
  TestSuppressOnlyDoesNotCrossBlockFontScopeStart();
  TestCssSpacingNearBottomAdvancesScreen();
  printf("PASS: %d tests\n", g_pass);
  return 0;
}
