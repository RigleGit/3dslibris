#include "book/book.h"
#include "book/book_context.h"
#include "book/book_xml.h"
#include "book/book_xml_block_handler.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_image_handler.h"
#include "book/book_xml_screen_advance.h"
#include "book/book_xml_text_emit.h"
#include "book/page.h"
#include "book_inline_image_stub_test_api.h"
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

void ExpectIntEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: expected %d got %d\n", label, expected, actual);
    g_fail++;
    std::exit(1);
  }
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

int LineStartXBefore(const u32 *buf, int len, u32 codepoint) {
  int last_x = -1;
  for (int i = 0; i < len; ++i) {
    if (buf[i] == TEXT_LINE_START_X && i + 1 < len) {
      last_x = (int)buf[++i];
    } else if (buf[i] == codepoint) {
      return last_x;
    }
  }
  return -1;
}

std::string ExtractVisibleAsciiFromPages(Book &book) {
  std::string out;
  for (int page_index = 0; page_index < book.GetPageCount(); page_index++) {
    Page *page = book.GetPage(page_index);
    if (!page)
      continue;
    const u32 *buf = page->GetBuffer();
    const int len = page->GetLength();
    for (int i = 0; i < len; i++) {
      const u32 c = buf[i];
      if (c == TEXT_LINE_START_X || c == TEXT_RTL_LINE_PX ||
          c == TEXT_FONT_SIZE || c == TEXT_LINK_START ||
          c == TEXT_IMAGE || c == TEXT_IMAGE_ALIGN ||
          c == TEXT_IMAGE_AUTHOR_WIDTH) {
        i++;
        continue;
      }
      if (c == TEXT_PARAGRAPH_RTL || c == TEXT_PARAGRAPH_LTR ||
          c == TEXT_PARAGRAPH_LEFT || c == TEXT_PARAGRAPH_CENTER ||
          c == TEXT_PARAGRAPH_RIGHT || c == TEXT_SCREEN_BREAK ||
          c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF ||
          c == TEXT_ITALIC_ON || c == TEXT_ITALIC_OFF ||
          c == TEXT_UNDERLINE_ON || c == TEXT_UNDERLINE_OFF ||
          c == TEXT_PRE_ON || c == TEXT_PRE_OFF ||
          c == TEXT_MONO_ON || c == TEXT_MONO_OFF ||
          c == TEXT_LINK_END || c == TEXT_IMAGE_CONTEXT_DEFAULT ||
          c == TEXT_IMAGE_LEADING_PARAGRAPH ||
          c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
        continue;
      }
      if (c >= 32 && c < 127)
        out.push_back((char)c);
    }
  }
  return out;
}

int TestMeasureCodepoint(uint32_t codepoint, void *) {
  return codepoint == ' ' ? 4 : 7;
}

struct TestCtx {
  Text text;
  BookContext ctx;
  unsigned char paragraph_spacing;
  unsigned char paragraph_indent;
  bool publisher_text_indent;
  bool publisher_block_margins;
  bool publisher_horizontal_margins;

  TestCtx() {
    ctx.text = &text;
    ctx.prefs = nullptr;
    ctx.status_reporter = nullptr;
    paragraph_spacing = 1;
    paragraph_indent = 0;
    publisher_text_indent = true;
    publisher_block_margins = true;
    publisher_horizontal_margins = true;
    ctx.paragraph_spacing = &paragraph_spacing;
    ctx.paragraph_indent = &paragraph_indent;
    ctx.publisher_text_indent = &publisher_text_indent;
    ctx.publisher_block_margins = &publisher_block_margins;
    ctx.publisher_horizontal_margins = &publisher_horizontal_margins;
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
  // margin-top:0.5em. The positive margin contributes one blank line; a
  // stale suppress-only flag must not add another across the font scope.
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
  // One block boundary newline plus one blank line for the explicit margin.
  // Any further newline would indicate stale spacing from the outer scope.
  ExpectTrue("suppress-font-scope: no extra blank line before inner paragraph",
             CountBufValue(buf, len, '\n') == 2);
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
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          false, 0, tc.text.margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(
              tc.text.margin.bottom));
  (void)line_step;
  p.pen.y = metrics.max_height - metrics.bottom_margin;

  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "p");

  ExpectTrue("css-spacing-bottom: advanced to next screen", p.screen == 1);
}

void TestOverflowContinuationKeepsParagraphBoundary() {
  // Regression from real EPUB trace: text overflow can leave the parser on
  // the next screen with linebegan=true but current_screen_has_drawable_content
  // still false.  That screen is not truly empty; the next paragraph must emit
  // a boundary instead of joining labels as "blackAbilities" or "tailsQueen".
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  p.buflen = 16;
  p.buf[0] = TEXT_SCREEN_BREAK;
  p.linebegan = true;
  p.current_screen_has_drawable_content = false;
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 1;
  p.pending_block_spacing_reason = "paragraph-top";
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = false;
  p.screen = 1;
  p.pen.y = 66;

  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "text");

  ExpectIntEq("overflow-continuation-boundary: stays on right screen",
              p.screen, 1);
  ExpectTrue("overflow-continuation-boundary: emits paragraph boundary",
             p.pen.y > 66);
}

void TestTextOverflowMarksNextScreenAsDrawable() {
  // Real Wings trace: a text segment can advance from the full-height left
  // screen to the compact right screen before it is emitted. The fresh right
  // screen must be marked non-empty after that emission, otherwise the next
  // paragraph treats it as top-of-page and drops CSS top spacing.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  p.screen = 0;
  p.pen.x = tc.text.margin.left;
  p.pen.y = 365;
  p.linebegan = false;
  p.current_screen_has_drawable_content = false;

  const char text[] = "Wars";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("text-overflow-drawable: shape text",
             text_layout_utils::ShapeTextRunUtf8(
                 text, strlen(text), nullptr, TestMeasureCodepoint, nullptr,
                 &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics metrics{};
  metrics.display_width = 240;
  metrics.base_margin_left = tc.text.margin.left;
  metrics.margin_left = tc.text.margin.left;
  metrics.margin_right = tc.text.margin.right;
  metrics.lineheight = tc.text.GetHeight();
  metrics.linespacing = tc.text.linespacing;
  metrics.spaceadvance = TestMeasureCodepoint(' ', nullptr);
  metrics.text_already_transformed = false;
  metrics.screen_max_height = 400;
  metrics.screen_bottom_margin =
      tc.text.margin.bottom +
      text_render_layout_utils::kFullReadingScreenFooterGuardPx;
  metrics.overflow_threshold =
      metrics.screen_max_height - metrics.screen_bottom_margin;
  metrics.text_indent_px = 0;

  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, false, std::vector<text_bidi_utils::BidiRun>(),
      metrics,
      [](parsedata_t *pd, int lh, void *) {
        book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
      },
      nullptr);

  ExpectIntEq("text-overflow-drawable: advanced to right screen", p.screen, 1);
  ExpectTrue("text-overflow-drawable: emitted visible text",
             p.linebegan);
  ExpectTrue("text-overflow-drawable: marks right screen non-empty",
             p.current_screen_has_drawable_content);
}

void TestCssTopSpacingKeepsRenderableSlotsAtKnownPens() {
  const int pens[] = {291, 320, 322, 333, 346};
  for (int i = 0; i < 5; i++) {
    TestCtx tc;
    Book book(tc.ctx);
    parsedata_t p = MakeParseData(tc, book);

    p.buflen = 1;
    p.buf[0] = 'A';
    p.linebegan = false;
    p.current_screen_has_drawable_content = true;
    p.pending_block_break = true;
    p.pending_block_spacing_lf = 1;
    p.pending_block_spacing_reason = "paragraph-top";
    p.pending_block_spacing_from_css = true;
    p.pending_block_spacing_advance_ok = false;
    p.screen = 0;
    p.pen.y = pens[i];

    book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "p");

    char label[96];
    snprintf(label, sizeof(label), "css-top-known-pen-%d: stays on screen",
             pens[i]);
    ExpectIntEq(label, p.screen, pens[i] == 346 ? 1 : 0);
    if (pens[i] == 291 || pens[i] == 320 || pens[i] == 322) {
      snprintf(label, sizeof(label), "css-top-known-pen-%d: emits spacing",
               pens[i]);
      ExpectIntEq(label, p.pen.y, pens[i] + tc.text.GetHeight() +
                                      tc.text.linespacing);
    } else if (pens[i] == 333) {
      snprintf(label, sizeof(label), "css-top-known-pen-%d: collapses spacing",
               pens[i]);
      ExpectIntEq(label, p.pen.y, pens[i]);
    } else if (pens[i] == 346) {
      snprintf(label, sizeof(label), "css-top-known-pen-%d: advances safely",
               pens[i]);
      ExpectIntEq(label, p.pen.y,
                  tc.text.margin.top + tc.text.GetHeight());
    }
  }
}

void TestCssTopSpacingPreservesVisualParagraphGapNearBottom() {
  // Wings tribe pages use .contentsentry { margin-top: 1% }. After the image
  // and separator fixes, that one-line CSS gap should be preserved whenever it
  // still leaves room for at least two renderable content baselines.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  p.buflen = 1;
  p.buf[0] = 'A';
  p.linebegan = false;
  p.current_screen_has_drawable_content = true;
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 1;
  p.pending_block_spacing_reason = "paragraph-top";
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = false;
  p.screen = 0;
  p.pen.y = 316;

  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "p");

  ExpectIntEq("css-top-visual-gap: stays on left screen", p.screen, 0);
  ExpectIntEq("css-top-visual-gap: emits one-line publisher spacing",
              p.pen.y, 316 + tc.text.GetHeight() + tc.text.linespacing);
}

void TestCssTopOneLineSpacingNearBottomKeepsTwoRenderableLines() {
  // Regression: FlushPending used floor((limit - pen_y) / line_step), which
  // under-counted the current baseline. At pen_y=333 with a 356px guarded
  // bottom limit and a 13px step, the renderer can still draw at 333 and 346,
  // so CSS paragraph-top spacing should collapse instead of advancing.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  p.buflen = 1;
  p.buf[0] = 'A';
  p.linebegan = false;
  p.current_screen_has_drawable_content = true;
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 1;
  p.pending_block_spacing_reason = "paragraph-top";
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = false;
  p.screen = 0;

  const int line_step = tc.text.GetHeight() + tc.text.linespacing;
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          false, 0, tc.text.margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(
              tc.text.margin.bottom));
  p.pen.y = metrics.max_height - metrics.bottom_margin - line_step;

  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "p");

  ExpectTrue("css-top-one-line: stays on current screen", p.screen == 0);
  ExpectTrue("css-top-one-line: preserves two baseline slots",
             p.pen.y == metrics.max_height - metrics.bottom_margin - line_step);
}

void TestInlineImageMarksScreenAsDrawableContent() {
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  InlineImageMetadata meta{};
  meta.ok = true;
  meta.width = 1920;
  meta.height = 1231;

  InlineImageLayoutPlan plan{};
  plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  plan.draw_width = 216;
  plan.draw_height = 138;
  plan.line_break_before = false;
  plan.advance_before = false;
  plan.consume_rest_of_screen = false;
  plan.vertical_space_after_draw = 139;
  plan.next_text_screen = 0;
  plan.page_breaks = 0;
  ConfigureBookInlineImageStub(meta, plan, true);

  p.docpath = "OPS/chapter.xhtml";
  p.pen.x = tc.text.margin.left;
  p.pen.y = tc.text.margin.top + tc.text.GetHeight();
  p.linebegan = false;
  p.current_screen_has_drawable_content = false;

  const char *attr[] = {"src", "images/pg11a.jpg", nullptr};
  epub_css_class_map::CssClassMargins elem_css{};
  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) {
    book_xml_screen_advance::Linefeed(pd);
  };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };

  HandleInlineImageStart(&p, &tc.text, attr, elem_css, fns);

  ExpectTrue("image-drawable: image token emitted",
             BufContains(p.buf, p.buflen, TEXT_IMAGE));
  ExpectTrue("image-drawable: screen marked non-empty after band image",
             p.current_screen_has_drawable_content);

  ResetBookInlineImageStubState();
}

void TestStyledBandImageWithoutMarginTopDoesNotAddDefaultTopSpace() {
  // Regression: a BAND image with a style attribute but no margin-top was
  // receiving a default top linefeed when another image preceded it. That extra
  // line accumulated in image-heavy spine pages and pushed following text to
  // the next screen.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  InlineImageMetadata meta{};
  meta.ok = true;
  meta.width = 194;
  meta.height = 8;

  InlineImageLayoutPlan plan{};
  plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  plan.draw_width = 194;
  plan.draw_height = 8;
  plan.line_break_before = false;
  plan.advance_before = false;
  plan.consume_rest_of_screen = false;
  plan.vertical_space_after_draw = 8;
  plan.next_text_screen = 0;
  plan.page_breaks = 0;
  ConfigureBookInlineImageStub(meta, plan, true);

  p.docpath = "OPS/chapter.xhtml";
  p.pen.x = tc.text.margin.left;
  p.pen.y = 100;
  p.linebegan = false;
  p.current_screen_has_drawable_content = true;
  parse_append_page_byte(&p, 'x');
  parse_append_page_byte(&p, TEXT_IMAGE);
  parse_append_page_byte(&p, 99);

  const char *attr[] = {"src", "images/pg18a.jpg", "style", "width:80%",
                        nullptr};
  epub_css_class_map::CssClassMargins elem_css{};
  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) {
    book_xml_screen_advance::Linefeed(pd);
  };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };

  HandleInlineImageStart(&p, &tc.text, attr, elem_css, fns);

  ExpectTrue("styled-band-no-margin-top: no default top linefeed",
             p.pen.y == 108);

  ResetBookInlineImageStubState();
}

void TestPostImageCssSpacingFlushesNearBottom() {
  // Regression guard: after a BAND image on the current screen, CSS pending
  // spacing for the next paragraph must still flush near bottom-of-screen.
  // Without drawable-content tracking from image emission, Phase 2 is skipped.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  InlineImageMetadata meta{};
  meta.ok = true;
  meta.width = 1920;
  meta.height = 1231;

  InlineImageLayoutPlan plan{};
  plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  plan.draw_width = 216;
  plan.draw_height = 32;
  plan.line_break_before = false;
  plan.advance_before = false;
  plan.consume_rest_of_screen = false;
  plan.vertical_space_after_draw = 0;
  plan.next_text_screen = 0;
  plan.page_breaks = 0;
  ConfigureBookInlineImageStub(meta, plan, true);

  p.docpath = "OPS/chapter.xhtml";
  p.screen = 0;
  p.linebegan = false;
  p.current_screen_has_drawable_content = false;

  const int line_step = tc.text.GetHeight() + tc.text.linespacing;
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          false, 0, tc.text.margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(
              tc.text.margin.bottom));
  const int target_usable = line_step;
  p.pen.x = tc.text.margin.left;
  p.pen.y = metrics.max_height - metrics.bottom_margin - target_usable;

  const char *img_attr[] = {"src", "images/pg11a.jpg", nullptr};
  epub_css_class_map::CssClassMargins elem_css{};
  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) {
    book_xml_screen_advance::Linefeed(pd);
  };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };

  HandleInlineImageStart(&p, &tc.text, img_attr, elem_css, fns);
  ExpectTrue("post-image-spacing: image marks screen as drawable",
             p.current_screen_has_drawable_content);

  const int pen_before_flush = p.pen.y;
  const int screen_before_flush = p.screen;
  book_xml_screen_advance::QueueBlockSpacingLines(
      &p, 2, "p", "paragraph-top-css", true);
  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(&p, "text");

  ExpectIntEq("post-image-spacing: flush stays on current screen",
              p.screen, screen_before_flush);
  ExpectIntEq("post-image-spacing: flush collapses optional CSS spacing",
              p.pen.y, pen_before_flush);

  ResetBookInlineImageStubState();
}

void TestDecorativeBandImageSuppressesWrapperMargins() {
  // Regression from Wings of Fire tribe pages: a thin title/separator image
  // inside <p class="centerimage2"> inherited margin:10%, costing one line
  // before and one line after the 10px ornament. The ornament itself is the
  // separator; its wrapper margins must not consume paragraph slots.
  TestCtx tc;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);

  InlineImageMetadata meta{};
  meta.ok = true;
  meta.width = 1200;
  meta.height = 55;

  InlineImageLayoutPlan plan{};
  plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  plan.draw_width = 216;
  plan.draw_height = 10;
  plan.line_break_before = false;
  plan.advance_before = false;
  plan.consume_rest_of_screen = false;
  plan.vertical_space_after_draw = 10;
  plan.next_text_screen = 0;
  plan.page_breaks = 0;
  ConfigureBookInlineImageStub(meta, plan, true);

  p.docpath = "OEBPS/Text/part0006.xhtml";
  p.screen = 0;
  p.in_paragraph = true;
  p.paragraph_has_content = false;
  p.linebegan = false;
  p.current_screen_has_drawable_content = true;
  p.pen.x = tc.text.margin.left;
  p.pen.y = 225;
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 1;
  p.pending_block_spacing_reason = "paragraph-top";
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = true;

  const char *img_attr[] = {"src", "../Images/image00182.jpeg", nullptr};
  epub_css_class_map::CssClassMargins elem_css{};
  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) {
    book_xml_screen_advance::Linefeed(pd);
  };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };

  HandleInlineImageStart(&p, &tc.text, img_attr, elem_css, fns);

  ExpectIntEq("decorative-band: only consumes drawn separator height",
              p.pen.y, 235);
  ExpectFalse("decorative-band: wrapper paragraph remains non-content",
              p.paragraph_has_content);
  ExpectFalse("decorative-band: clears pending wrapper spacing",
              p.pending_block_break);
  ExpectIntEq("decorative-band: clears pending optional spacing",
              p.pending_block_spacing_lf, 0);

  ResetBookInlineImageStubState();
}

void TestBandImageSeparatorSequenceMatchesRenderedHeight() {
  // Wings of Fire tribe pages use one paragraph for the dragon image, another
  // for the title/separator image, then a text paragraph. The renderer consumes
  // those two band image heights back-to-back, so parser state must do the same.
  TestCtx tc;
  tc.paragraph_spacing = 0;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  p.docpath = "OEBPS/Text/part0006.xhtml";
  p.screen = 0;
  p.pen.x = tc.text.margin.left;
  p.pen.y = 24;

  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) {
    book_xml_screen_advance::Linefeed(pd);
  };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };

  epub_css_class_map::CssClassMargins elem_css{};
  const char *image_p_attr[] = {"style", "margin:10% 0", nullptr};
  const char *text_p_attr[] = {"style", "margin-top:1%;margin-bottom:0", nullptr};

  InlineImageMetadata main_meta{};
  main_meta.ok = true;
  main_meta.width = 1200;
  main_meta.height = 1040;
  InlineImageLayoutPlan main_plan{};
  main_plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  main_plan.draw_width = 216;
  main_plan.draw_height = 187;
  main_plan.vertical_space_after_draw = 187;
  ConfigureBookInlineImageStub(main_meta, main_plan, true);

  bool early = false;
  book_xml_block_handler::HandleBlockElementStart(&p, &tc.text, "p",
                                                  image_p_attr, elem_css, "",
                                                  &early);
  const char *main_img_attr[] = {"src", "../Images/image00181.jpeg", nullptr};
  HandleInlineImageStart(&p, &tc.text, main_img_attr, elem_css, fns);
  book_xml_block_handler::HandleBlockElementEnd(&p, &tc.text, "p");
  parse_pop(&p);
  ExpectIntEq("band-sequence: main image consumes only draw height", p.pen.y, 211);
  ExpectTrue("band-sequence: remembers image-only paragraph",
             p.last_block_was_standalone_band_image);

  InlineImageMetadata sep_meta{};
  sep_meta.ok = true;
  sep_meta.width = 1200;
  sep_meta.height = 55;
  InlineImageLayoutPlan sep_plan{};
  sep_plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  sep_plan.draw_width = 216;
  sep_plan.draw_height = 10;
  sep_plan.vertical_space_after_draw = 10;
  ConfigureBookInlineImageStub(sep_meta, sep_plan, true);

  early = false;
  book_xml_block_handler::HandleBlockElementStart(&p, &tc.text, "p",
                                                  image_p_attr, elem_css, "",
                                                  &early);
  ExpectIntEq("band-sequence: separator starts without parser-only linefeed",
              p.pen.y, 211);
  const char *sep_img_attr[] = {"src", "../Images/image00182.jpeg", nullptr};
  HandleInlineImageStart(&p, &tc.text, sep_img_attr, elem_css, fns);
  book_xml_block_handler::HandleBlockElementEnd(&p, &tc.text, "p");
  parse_pop(&p);
  ExpectIntEq("band-sequence: separator consumes only draw height", p.pen.y, 221);
  ExpectTrue("band-sequence: remembers separator-only paragraph",
             p.last_block_was_standalone_band_image);

  early = false;
  book_xml_block_handler::HandleBlockElementStart(&p, &tc.text, "p",
                                                  text_p_attr, elem_css, "",
                                                  &early);
  ExpectIntEq("band-sequence: text starts immediately after separator",
              p.pen.y, 221);
  ExpectFalse("band-sequence: consumed image-only suppression",
              p.last_block_was_standalone_band_image);
  ExpectFalse("band-sequence: no pending top spacing after separator",
              p.pending_block_break);
  ExpectIntEq("band-sequence: no optional top spacing after separator",
              p.pending_block_spacing_lf, 0);

  ResetBookInlineImageStubState();
}

void TestBodyTextIndentIsInheritedAndClassZeroOverrides() {
  TestCtx tc;
  tc.paragraph_spacing = 0;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  const char *css = "body{text-indent:1.65em}.left{text-indent:0}";
  epub_css_class_map::ParseCssIntoClassMap(css, strlen(css), &p.css_class_map);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body><p class=\"left\">Alpha</p><p>Bravo</p></body></html>";
  const xml_parse_utils::XmlParseResult r =
      xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("body-indent: parse ok", r.ok);
  ExpectTrue("body-indent: page produced", book.GetPageCount() > 0);

  const u32 *buf = book.GetPage(0)->GetBuffer();
  const int len = book.GetPage(0)->GetLength();
  ExpectIntEq("body-indent: class zero keeps first paragraph flush",
              LineStartXBefore(buf, len, 'A'), -1);
  ExpectTrue("body-indent: following paragraph inherits body indent",
             LineStartXBefore(buf, len, 'B') > tc.text.margin.left);
}

void TestImageOnlyParagraphKeepsExplicitBottomMargin() {
  TestCtx tc;
  tc.paragraph_spacing = 0;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  const char *css = ".artpgbrkabovespacebelow{margin-bottom:4em}";
  epub_css_class_map::ParseCssIntoClassMap(css, strlen(css), &p.css_class_map);
  p.docpath = "OEBPS/Text/prologue.xhtml";
  p.screen = 0;
  p.pen.x = tc.text.margin.left;
  p.pen.y = 24;

  InlineImageMetadata meta{};
  meta.ok = true;
  meta.width = 900;
  meta.height = 240;
  InlineImageLayoutPlan plan{};
  plan.mode = INLINE_IMAGE_LAYOUT_BAND;
  plan.draw_width = 216;
  plan.draw_height = 58;
  plan.vertical_space_after_draw = 58;
  ConfigureBookInlineImageStub(meta, plan, true);

  epub_css_class_map::CssClassMargins elem_css{};
  elem_css.margin_bottom.unit =
      book_xml_css_style_utils::MarginTopResult::Unit::Em;
  elem_css.margin_bottom.value = 400;
  const char *p_attr[] = {"class", "artpgbrkabovespacebelow", nullptr};
  bool early = false;
  book_xml_block_handler::HandleBlockElementStart(
      &p, &tc.text, "p", p_attr, elem_css,
      "artpgbrkabovespacebelow", &early);

  ImageHandlerFns fns{};
  fns.linefeed = [](parsedata_t *pd) { book_xml_screen_advance::Linefeed(pd); };
  fns.advance_screen = [](parsedata_t *pd) {
    book_xml_screen_advance::AdvanceParsedScreen(pd);
  };
  fns.advance_page_overflow = [](parsedata_t *pd, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(pd, lh);
  };
  fns.emit_chardata = [](parsedata_t *pd, const char *txt, int len) {
    xml::book::chardata(pd, txt, len);
  };
  const char *img_attr[] = {"src", "../Images/prologue.jpg", nullptr};
  HandleInlineImageStart(&p, &tc.text, img_attr, elem_css, fns);
  book_xml_block_handler::HandleBlockElementEnd(&p, &tc.text, "p");

  ExpectTrue("image-bottom-margin: queues explicit spacing",
             p.pending_block_spacing_from_css);
  ExpectTrue("image-bottom-margin: keeps substantial 4em gap",
             p.pending_block_spacing_lf >= 3);
  ResetBookInlineImageStubState();
}

void TestPublisherMarginsAreIndependent() {
  for (bool vertical : {false, true}) {
    for (bool horizontal : {false, true}) {
      for (bool per_book : {false, true}) {
        TestCtx tc;
        tc.paragraph_spacing = 0;
        tc.publisher_block_margins = per_book ? !vertical : vertical;
        tc.publisher_horizontal_margins = per_book ? !horizontal : horizontal;
        Book book(tc.ctx);
        if (per_book) {
          book.SetStylePublisherBlockMarginsOverride(vertical ? 1 : 0);
          book.SetStylePublisherHorizontalMarginsOverride(horizontal ? 1 : 0);
        }
        parsedata_t p = MakeParseData(tc, book);
        p.linebegan = true;
        const char *attr[] = {
            "style", "margin-left:48px;margin-right:24px;margin-top:48px",
            nullptr};
        epub_css_class_map::CssClassMargins elem_css{};
        bool early = false;
        book_xml_block_handler::HandleBlockElementStart(
            &p, &tc.text, "blockquote", attr, elem_css, "", &early);
        ExpectIntEq("publisher left margin follows side setting",
                    parse_current_block_margin_left(&p), horizontal ? 48 : 0);
        ExpectIntEq("publisher right margin follows side setting",
                    parse_current_block_margin_right(&p), horizontal ? 24 : 0);
        ExpectTrue("publisher vertical spacing is independent of sides",
                   (p.pending_block_spacing_lf > 0) == vertical);
      }
    }
  }
}

void TestPageBreakBeforeAlwaysUsesHardBreak() {
  TestCtx tc;
  tc.paragraph_spacing = 0;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  const std::string html =
      "<html><body>"
      "<p>entry one</p>"
      "<div style=\"page-break-before:always;padding-top:10%\">entry two</div>"
      "</body></html>";

  xml_parse_utils::XmlParseResult r = xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("hard-break: parse ok", r.ok);
  ExpectTrue("hard-break: creates second logical page", book.GetPageCount() >= 2);
}

void TestLargeFontPaginationDoesNotDropTextAcrossPages() {
  TestCtx tc;
  tc.text.SetPixelSize(20);
  tc.text.linespacing = 2;
  tc.paragraph_spacing = 0;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  xml_parse_utils::XmlParserOptions opts = MakeXmlOpts(&p);

  std::string expected;
  std::string html = "<html><body><p>";
  for (int i = 0; i < 220; i++) {
    char word[32];
    snprintf(word, sizeof(word), "w%03d", i);
    if (i > 0) {
      html += " ";
      expected += " ";
    }
    html += word;
    expected += word;
  }
  html += "</p></body></html>";

  const xml_parse_utils::XmlParseResult r =
      xml_parse_utils::ParseXmlString(html, opts);
  ExpectTrue("large-font-continuity: parse ok", r.ok);
  ExpectTrue("large-font-continuity: spans multiple pages",
             book.GetPageCount() > 1);

  const std::string actual = ExtractVisibleAsciiFromPages(book);
  ExpectTrue("large-font-continuity: all text preserved",
             actual.find(expected) != std::string::npos);
}

} // namespace

#include "xml_page_rendering_cases.h"

int main() {
  TestEmbeddedCssParagraphSpacing();
  TestAdjacentDivsStartSeparateLines();
  TestEmbeddedCssAlignedLines();
  TestXmlAlignedLines();
  TestXmlPageRenderingContinuity();
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
  TestOverflowContinuationKeepsParagraphBoundary();
  TestTextOverflowMarksNextScreenAsDrawable();
  TestCssTopSpacingKeepsRenderableSlotsAtKnownPens();
  TestCssTopSpacingPreservesVisualParagraphGapNearBottom();
  TestCssTopOneLineSpacingNearBottomKeepsTwoRenderableLines();
  TestInlineImageMarksScreenAsDrawableContent();
  TestStyledBandImageWithoutMarginTopDoesNotAddDefaultTopSpace();
  TestPostImageCssSpacingFlushesNearBottom();
  TestDecorativeBandImageSuppressesWrapperMargins();
  TestBandImageSeparatorSequenceMatchesRenderedHeight();
  TestBodyTextIndentIsInheritedAndClassZeroOverrides();
  TestImageOnlyParagraphKeepsExplicitBottomMargin();
  TestPublisherMarginsAreIndependent();
  TestPageBreakBeforeAlwaysUsesHardBreak();
  TestLargeFontPaginationDoesNotDropTextAcrossPages();
  printf("PASS: %d tests\n", g_pass);
  return 0;
}
