#include "book/book_xml_text_emit.h"

#include "parse.h"
#include "shared/text_layout_utils.h"
#include "shared/text_token_constants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

[[noreturn]] void Fail(const std::string &message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

int MeasureMono(uint32_t, void *) { return 1; }

book_xml_text_emit::FlowEmitMetrics BaseMetrics() {
  book_xml_text_emit::FlowEmitMetrics metrics{};
  metrics.display_width = 6;
  metrics.base_margin_left = 0;
  metrics.margin_left = 0;
  metrics.margin_right = 0;
  metrics.lineheight = 10;
  metrics.linespacing = 1;
  metrics.spaceadvance = 1;
  return metrics;
}

int MeasureUppercaseWide(uint32_t cp, void *) {
  if (cp >= 'A' && cp <= 'Z')
    return 2;
  return 1;
}

void TestEmitFlowedLtrWrapsIntoPageBuffer() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 10;

  const char *text = "one two";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape ltr",
             text_layout_utils::ShapeTextRunBidi(text, 7, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, bidi_runs, BaseMetrics(), NULL, NULL);

  ExpectEq("buflen", p.buflen, 8);
  ExpectEq("newline inserted", (int)p.buf[4], '\n');
  ExpectEq("last char", (int)p.buf[7], 'o');
  ExpectTrue("linebegan true", p.linebegan);
  ExpectEq("pen x moved to second word width", p.pen.x, 3);
}

void TestSearchContinuityAcrossVisualWrap() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.y = 10;
  const char *text = "abcdefghij";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool rtl = false;
  text_layout_utils::ShapeTextRunBidi(text, 10, NULL, MeasureMono, NULL, &run, &rtl);
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, rtl, std::vector<text_bidi_utils::BidiRun>(),
      BaseMetrics(), NULL, NULL);
  std::string logical;
  for (int i = 0; i < p.buflen; ++i) {
    if (!(page_search::FlagsAt(p.search_events, i) & page_search::Skip))
      logical += (char)p.buf[i];
  }
  ExpectTrue("wrapped word remains searchable", logical == text);
}

void TestEmitFlowedRtlAddsParagraphAndWidthTokens() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 10;
  p.in_paragraph = true;

  const char *text =
      "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD9\x83\xD9\x85";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape rtl",
             text_layout_utils::ShapeTextRunBidi(text, std::strlen(text), NULL,
                                                 MeasureMono, NULL, &run,
                                                 &has_rtl));
  ExpectTrue("rtl detected", has_rtl);

  std::vector<uint32_t> cps;
  for (size_t i = 0; i < run.size(); i++)
    cps.push_back(run[i].text.codepoint);
  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  ExpectTrue("analyze bidi runs",
             text_bidi_utils::AnalyzeBidiRuns(cps.data(), cps.size(),
                                              &bidi_runs));

  book_xml_text_emit::FlowEmitMetrics metrics = BaseMetrics();
  metrics.display_width = 40;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, bidi_runs, metrics, NULL, NULL);

  ExpectEq("paragraph token", (int)p.buf[0], TEXT_PARAGRAPH_RTL);
  ExpectEq("rtl width token", (int)p.buf[1], TEXT_RTL_LINE_PX);
  ExpectEq("rtl width value", (int)p.buf[2], 9);
  ExpectTrue("paragraph marked with content", p.paragraph_has_content);
  ExpectTrue("rtl emitted glyphs", p.buflen > 3);
  ExpectEq("pen x equals rtl line width", p.pen.x, 9);
}

void TestEmitKeepsClosingPunctuationOnCurrentLine() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 6;
  p.pen.y = 10;
  p.linebegan = true;

  const char *text = "!";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape punctuation",
             text_layout_utils::ShapeTextRunBidi(text, 1, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      BaseMetrics(), NULL, NULL);

  ExpectEq("punctuation emitted without forced newline", p.buflen, 1);
  ExpectEq("punctuation token", (int)p.buf[0], '!');
  ExpectEq("pen advances on same line", p.pen.x, 7);
}

void TestEmitStartsLtrLineAtLeftMargin() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 10;

  const char *text = "abc";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape margin text",
             text_layout_utils::ShapeTextRunBidi(text, 3, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics metrics = BaseMetrics();
  metrics.display_width = 12;
  metrics.margin_left = 4;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), metrics,
      NULL, NULL);

  ExpectEq("buflen", p.buflen, 5);
  ExpectEq("line start token", (int)p.buf[0], TEXT_LINE_START_X);
  ExpectEq("line start x", (int)p.buf[1], 4);
  ExpectEq("first char", (int)p.buf[2], 'a');
  ExpectEq("pen starts from left margin", p.pen.x, 7);
}

void TestEmitClampsMarginAtBaseMarginLeft() {
  // margin_left < base_margin_left: pen.x is clamped to base_margin_left,
  // so text never renders left of the global margin.
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 8;
  p.pen.y = 10;

  const char *text = "abc";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape shifted margin text",
             text_layout_utils::ShapeTextRunBidi(text, 3, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics metrics = BaseMetrics();
  metrics.display_width = 20;
  metrics.base_margin_left = 8;
  metrics.margin_left = 3;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), metrics,
      NULL, NULL);

  // pen.x stays at base_margin_left (8); no TEXT_LINE_START_X emitted.
  ExpectEq("buflen", p.buflen, 3);
  ExpectEq("first char", (int)p.buf[0], 'a');
  ExpectEq("pen clamped to base margin", p.pen.x, 11);
}

void TestEmitRepeatsLeftMarginTokenAfterWrap() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 10;

  const char *text = "one two";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape wrapped margin text",
             text_layout_utils::ShapeTextRunBidi(text, 7, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics metrics = BaseMetrics();
  metrics.display_width = 8;
  metrics.margin_left = 2;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), metrics,
      NULL, NULL);

  ExpectEq("buflen", p.buflen, 12);
  ExpectEq("first line token", (int)p.buf[0], TEXT_LINE_START_X);
  ExpectEq("first line x", (int)p.buf[1], 2);
  ExpectEq("newline inserted", (int)p.buf[6], '\n');
  ExpectEq("second line token", (int)p.buf[7], TEXT_LINE_START_X);
  ExpectEq("second line x", (int)p.buf[8], 2);
  ExpectEq("second line first char", (int)p.buf[9], 't');
}

void TestTransformedTextIsMeasuredBeforeFlowing() {
  parsedata_t p{};
  parse_init(&p);
  parse_push(&p, TAG_P);
  p.style_text_transform_stack[p.stacksize - 1] = 1; // uppercase
  p.pen.x = 0;
  p.pen.y = 10;

  const char *text = "ab c";
  const std::string transformed =
      book_xml_text_emit::TransformUtf8ForLayout(&p, text, std::strlen(text));
  ExpectTrue("text transformed for layout", transformed == "AB C");

  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape transformed text",
             text_layout_utils::ShapeTextRunBidi(
                 transformed.c_str(), transformed.size(), NULL,
                 MeasureUppercaseWide, NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics metrics = BaseMetrics();
  metrics.display_width = 6;
  metrics.text_already_transformed = true;
  book_xml_text_emit::EmitFlowedShapedText(
      &p, transformed.c_str(), run, has_rtl,
      std::vector<text_bidi_utils::BidiRun>(), metrics, NULL, NULL);

  ExpectEq("buflen", p.buflen, 5);
  ExpectEq("first transformed char", (int)p.buf[0], 'A');
  ExpectEq("second transformed char", (int)p.buf[1], 'B');
  ExpectEq("space preserved before wrap", (int)p.buf[2], ' ');
  ExpectEq("newline inserted after transformed width", (int)p.buf[3], '\n');
  ExpectEq("wrapped transformed char", (int)p.buf[4], 'C');
}

// ---------------------------------------------------------------------------
// Punctuation glue tests.
// ---------------------------------------------------------------------------

// Mock overflow context for tests that need to verify advance-page behavior.
struct MockOverflowCtx {
  bool fired;
};

static void MockAdvancePage(parsedata_t *p, int lineheight, void *ctx) {
  auto *m = static_cast<MockOverflowCtx *>(ctx);
  m->fired = true;
  // Simulate fresh screen: pen at top.
  p->pen.y = lineheight;  // margin.top=0 + lineheight
  p->pen.x = 0;
}

static void MockAdvanceReadingScreen(parsedata_t *p, int lineheight,
                                     void *ctx) {
  MockAdvancePage(p, lineheight, ctx);
  p->screen = 1;
}

static book_xml_text_emit::FlowEmitMetrics PerScreenMetrics() {
  book_xml_text_emit::FlowEmitMetrics m = BaseMetrics();
  m.display_width = 8;
  m.per_screen_valid = true;
  m.screen_width_by_screen[0] = 8;
  m.screen_width_by_screen[1] = 5;
  m.screen_max_height_by_screen[0] = 20;
  m.screen_max_height_by_screen[1] = 20;
  m.screen_bottom_margin_by_screen[0] = 0;
  m.screen_bottom_margin_by_screen[1] = 0;
  return m;
}

void TestLandscapeLtrRemeasuresAfterScreenAdvance() {
  parsedata_t p{};
  parse_init(&p);
  p.screen = 0;
  p.pen.y = 20;

  const char *text = "abcdefgh";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape landscape ltr",
             text_layout_utils::ShapeTextRunBidi(text, 8, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      PerScreenMetrics(), MockAdvanceReadingScreen, &ctx);

  ExpectTrue("landscape ltr advances to second screen", ctx.fired);
  ExpectEq("landscape ltr current screen", p.screen, 1);
  ExpectEq("landscape ltr uses second-screen width", p.pen.x, 3);
}

void TestLandscapeRtlMeasuresAfterScreenAdvance() {
  parsedata_t p{};
  parse_init(&p);
  p.screen = 0;
  p.pen.y = 20;
  p.in_paragraph = true;

  const char *text = "\xD8\xA7\xD8\xA8\xD8\xAC\xD8\xAF\xD9\x87\xD9\x88";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape landscape rtl",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));
  ExpectTrue("landscape rtl detected", has_rtl);

  std::vector<uint32_t> cps;
  for (size_t i = 0; i < run.size(); i++)
    cps.push_back(run[i].text.codepoint);
  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  ExpectTrue("analyze landscape rtl",
             text_bidi_utils::AnalyzeBidiRuns(cps.data(), cps.size(),
                                              &bidi_runs));

  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, bidi_runs, PerScreenMetrics(),
      MockAdvanceReadingScreen, &ctx);

  ExpectTrue("landscape rtl advances to second screen", ctx.fired);
  ExpectEq("landscape rtl current screen", p.screen, 1);
  ExpectEq("landscape rtl width token", (int)p.buf[2], 5);
}

// Metrics for overflow threshold tests.
// display_width=6, lineheight=13, linespacing=2 → step=15, threshold=304.
static book_xml_text_emit::FlowEmitMetrics ThresholdMetrics() {
  book_xml_text_emit::FlowEmitMetrics m = BaseMetrics();
  m.lineheight = 13;
  m.linespacing = 2;
  m.overflow_threshold = 304;
  return m;
}

// Helper: emit "word" into parsedata to prime the state (pen.x=4, linebegan).
static void PrimeWithWord(parsedata_t *p) {
  const char *text = "word";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  book_xml_text_emit::FlowEmitMetrics m{};
  m.display_width = 80;
  m.base_margin_left = 0;
  m.margin_left = 0;
  m.margin_right = 0;
  m.lineheight = 10;
  m.linespacing = 1;
  m.spaceadvance = 1;
  ExpectTrue("prime shape",
             text_layout_utils::ShapeTextRunBidi(text, 4, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));
  book_xml_text_emit::EmitFlowedShapedText(
      p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), m,
      NULL, NULL);
}

// Test: a trailing space already in the buffer before a period is removed
// (retroactive glue). Simulates a collapsed newline/space at inline boundary
// followed by "." in the next run.
//
// Without fix: buf = "word " + ". rest" → buf[4]=' ', buf[5]='.'
// With fix:    buf = "word"  + ". rest" → buf[4]='.'   (space removed)
void TestPunctGlueNoSpaceBeforePeriod() {
  parsedata_t p{};
  parse_init(&p);
  PrimeWithWord(&p);  // buf = ['w','o','r','d'], pen.x=4, linebegan=true

  // Manually append a trailing space (simulating a collapsed newline from a
  // previous inline run ending with whitespace).
  parse_append_page_byte(&p, (u32)' ');
  p.pen.x += 1; // spaceadvance=1

  // "." followed by space — period is its own segment (line-breakable after).
  const char *text = ". rest";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  book_xml_text_emit::FlowEmitMetrics m{};
  m.display_width = 80;
  m.base_margin_left = 0;
  m.margin_left = 0;
  m.margin_right = 0;
  m.lineheight = 10;
  m.linespacing = 1;
  m.spaceadvance = 1;
  ExpectTrue("shape period run",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), m,
      NULL, NULL);

  // The trailing space should have been removed before '.'.
  // buf = ['w','o','r','d', '.', ' ', 'r', 'e', 's', 't']
  //   buf[4] must be '.' not ' '.
  ExpectEq("trailing space removed before period", (int)p.buf[4], '.');
}

// Test: a trailing space before a comma is removed (retroactive glue).
void TestPunctGlueNoSpaceBeforeComma() {
  parsedata_t p{};
  parse_init(&p);
  PrimeWithWord(&p);

  parse_append_page_byte(&p, (u32)' ');
  p.pen.x += 1;

  const char *text = ", rest";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  book_xml_text_emit::FlowEmitMetrics m{};
  m.display_width = 80;
  m.base_margin_left = 0;
  m.margin_left = 0;
  m.margin_right = 0;
  m.lineheight = 10;
  m.linespacing = 1;
  m.spaceadvance = 1;
  ExpectTrue("shape comma run",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(), m,
      NULL, NULL);

  // Trailing space removed before ','.
  ExpectEq("trailing space removed before comma", (int)p.buf[4], ',');
}

// Test: a comma at the right edge of the display does NOT force a line break
// (same guard as '!' — extended IsClosingAttachedPunctuation).
void TestPunctGlueNoLineBreakBeforeComma() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 6; // at display_width=6 boundary (BaseMetrics)
  p.pen.y = 10;
  p.linebegan = true;

  const char *text = ",";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape comma",
             text_layout_utils::ShapeTextRunBidi(text, 1, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      BaseMetrics(), NULL, NULL);

  // Comma emitted without forced newline (same as '!' behavior).
  ExpectEq("comma emitted without newline", p.buflen, 1);
  ExpectEq("comma token", (int)p.buf[0], ',');
  ExpectEq("pen advances on same line", p.pen.x, 7);
}

// Test: a period at the right edge of the display does NOT force a line break.
void TestPunctGlueNoLineBreakBeforePeriod() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 6;
  p.pen.y = 10;
  p.linebegan = true;

  const char *text = ".";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape period",
             text_layout_utils::ShapeTextRunBidi(text, 1, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      BaseMetrics(), NULL, NULL);

  ExpectEq("period emitted without newline", p.buflen, 1);
  ExpectEq("period token", (int)p.buf[0], '.');
  ExpectEq("pen advances on same line", p.pen.x, 7);
}

// ---------------------------------------------------------------------------
// LTR wrap invariant: last visible line tests.
// ---------------------------------------------------------------------------

// Regression: wrapping from y=279 to y=294 (y=294 <= threshold=304) must NOT
// fire AdvancePageIfNeeded. The old code fired WouldOverflow(294) = 309 > 304.
void TestLastVisibleLineIsUsed() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 4;   // 2 remaining pixels in 6-wide display
  p.pen.y = 279;
  p.linebegan = true;

  const char *text = "de"; // width=2 with MeasureMono, 4+2=6 >= 6 → wraps
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape de",
             text_layout_utils::ShapeTextRunBidi(text, 2, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("advance NOT fired for last visible line (y=294 <= 304)", !ctx.fired);
  ExpectEq("pen.y wraps to 294, not advanced away", p.pen.y, 294);
}

// Regression counterpart: wrapping from y=294 to y=309 (y=309 > threshold=304)
// MUST fire AdvancePageIfNeeded.
void TestBeyondLastVisibleLineAdvances() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 4;
  p.pen.y = 294; // one more step would be 309 > 304
  p.linebegan = true;

  const char *text = "de";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape de",
             text_layout_utils::ShapeTextRunBidi(text, 2, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("advance FIRED when wrap goes beyond threshold (309 > 304)",
             ctx.fired);
}

void TestContinuationWrapDoesNotUseCompactClipGuard() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 200;
  p.pen.y = 291;
  p.linebegan = true;
  p.in_paragraph = true;
  p.paragraph_has_content = true;

  const char *text = "p.mt-class-qualified";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape qualified class text",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  m.screen_max_height = 320;
  m.screen_bottom_margin = 16;
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("advance fired before compact clip guard", ctx.fired);
  ExpectEq("wrapped continuation starts on fresh screen", p.pen.y,
           m.lineheight);
}

void TestContinuationWrapBeyondCompactBleedAdvances() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 200;
  p.pen.y = 294;
  p.linebegan = true;
  p.in_paragraph = true;
  p.paragraph_has_content = true;

  const char *text = "p.mt-class-qualified";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape qualified class text beyond bleed",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  m.screen_max_height = 320;
  m.screen_bottom_margin = 16;
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("advance fires when wrapped line exceeds compact bleed",
             ctx.fired);
}

// Chapter 19 regression: a phrase starting at y=279 where the first word
// wraps to y=294 must NOT be displaced to the next screen/page.
// Both the wrapping word and subsequent words on y=294 must stay on the
// same screen (y=294 is the last visible line).
void TestChapter19DeVueltaNotSplit() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 4;   // forces "de" to wrap
  p.pen.y = 279;
  p.linebegan = true;

  // "de ab": "de" wraps to y=294; "ab" then fits on y=294.
  const char *text = "de ab";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape de ab",
             text_layout_utils::ShapeTextRunBidi(text, 5, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("no advance fired: last visible line used for both words",
             !ctx.fired);
  ExpectEq("pen.y stayed at 294 after full phrase", p.pen.y, 294);
}

// Chapter 11 regression: "Nintendo" wrapping from y=279 to y=294
// must NOT fire AdvancePageIfNeeded.
// "Nintendo" = 8px with MeasureMono. display_width must be > 8 (fits from x=0)
// but < pen.x + 8 (forces wrap). Use display_width=10, pen.x=5.
void TestChapter11NintendoNotAdvanced() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 5;   // 5+8=13 >= 10 → wraps; from x=0: 0+8=8 < 10 → fits
  p.pen.y = 279;
  p.linebegan = true;

  const char *text = "Nintendo";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape Nintendo",
             text_layout_utils::ShapeTextRunBidi(text, 8, NULL, MeasureMono,
                                                 NULL, &run, &has_rtl));

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 10; // wide enough for the 8px word from x=0
  MockOverflowCtx ctx{};
  book_xml_text_emit::EmitFlowedShapedText(
      &p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, &ctx);

  ExpectTrue("advance NOT fired for Nintendo wrap to y=294", !ctx.fired);
  ExpectEq("pen.y is 294 after Nintendo wrap", p.pen.y, 294);
}

void EmitTextRun(parsedata_t *p, const char *text,
                 const book_xml_text_emit::FlowEmitMetrics &m,
                 MockOverflowCtx *ctx) {
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  ExpectTrue("shape run",
             text_layout_utils::ShapeTextRunBidi(
                 text, std::strlen(text), NULL, MeasureMono, NULL, &run,
                 &has_rtl));
  book_xml_text_emit::EmitFlowedShapedText(
      p, text, run, has_rtl, std::vector<text_bidi_utils::BidiRun>(),
      m, MockAdvancePage, ctx);
}

void TestPunctuationRunStaysWithPreviousTokenOnLastVisibleLine() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 298;

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  MockOverflowCtx ctx{};

  EmitTextRun(&p, "64px", m, &ctx);
  ExpectEq("64px y stays on last visible line", p.pen.y, 298);
  ExpectTrue("64px does not advance", !ctx.fired);

  EmitTextRun(&p, ".", m, &ctx);
  ExpectTrue("period does not advance away from previous token", !ctx.fired);
  ExpectEq("period remains on last visible line", p.pen.y, 298);
  ExpectEq("period attached after 64px", (int)p.buf[4], '.');
}

void TestInlineStylePeriodRunHasNoSpaceOrScreenBreak() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 298;

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  MockOverflowCtx ctx{};

  EmitTextRun(&p, "em", m, &ctx);
  parse_append_page_byte(&p, (u32)' ');
  p.pen.x += m.spaceadvance;
  EmitTextRun(&p, ".", m, &ctx);

  ExpectTrue("period after inline style does not advance", !ctx.fired);
  ExpectEq("space removed before inline period", (int)p.buf[2], '.');
}

void TestInlineSpanCommaRunHasNoSpaceOrScreenBreak() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 298;

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  MockOverflowCtx ctx{};

  EmitTextRun(&p, "reset", m, &ctx);
  parse_append_page_byte(&p, (u32)' ');
  p.pen.x += m.spaceadvance;
  EmitTextRun(&p, ", y", m, &ctx);

  ExpectTrue("comma after span does not advance", !ctx.fired);
  ExpectEq("space removed before inline comma", (int)p.buf[5], ',');
}

void TestNewRunWordUsesCurrentVisibleLineIfItFits() {
  parsedata_t p{};
  parse_init(&p);
  p.pen.x = 0;
  p.pen.y = 294;

  book_xml_text_emit::FlowEmitMetrics m = ThresholdMetrics();
  m.display_width = 216;
  MockOverflowCtx ctx{};

  EmitTextRun(&p, "Capitalize", m, &ctx);
  ExpectEq("Capitalize y stays at last visible line", p.pen.y, 294);
  ExpectTrue("Capitalize does not advance", !ctx.fired);

  EmitTextRun(&p, " Via Clase", m, &ctx);
  ExpectTrue("new word run does not advance when it fits", !ctx.fired);
  ExpectEq("new word run remains on current visible line", p.pen.y, 294);
}

} // namespace

int main() {
  TestEmitFlowedLtrWrapsIntoPageBuffer();
  TestSearchContinuityAcrossVisualWrap();
  TestEmitFlowedRtlAddsParagraphAndWidthTokens();
  TestEmitKeepsClosingPunctuationOnCurrentLine();
  TestEmitStartsLtrLineAtLeftMargin();
  TestEmitClampsMarginAtBaseMarginLeft();
  TestEmitRepeatsLeftMarginTokenAfterWrap();
  TestTransformedTextIsMeasuredBeforeFlowing();
  // Punctuation glue tests.
  TestPunctGlueNoSpaceBeforePeriod();
  TestPunctGlueNoSpaceBeforeComma();
  TestPunctGlueNoLineBreakBeforeComma();
  TestPunctGlueNoLineBreakBeforePeriod();
  // LTR wrap invariant: last visible line tests.
  TestLastVisibleLineIsUsed();
  TestBeyondLastVisibleLineAdvances();
  TestContinuationWrapDoesNotUseCompactClipGuard();
  TestContinuationWrapBeyondCompactBleedAdvances();
  TestChapter19DeVueltaNotSplit();
  TestChapter11NintendoNotAdvanced();
  TestPunctuationRunStaysWithPreviousTokenOnLastVisibleLine();
  TestInlineStylePeriodRunHasNoSpaceOrScreenBreak();
  TestInlineSpanCommaRunHasNoSpaceOrScreenBreak();
  TestNewRunWordUsesCurrentVisibleLineIfItFits();
  TestLandscapeLtrRemeasuresAfterScreenAdvance();
  TestLandscapeRtlMeasuresAfterScreenAdvance();
  return 0;
}
