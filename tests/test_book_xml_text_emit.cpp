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

void TestEmitMovesFreshLineFromBaseToEffectiveLeftMargin() {
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

  ExpectEq("buflen", p.buflen, 5);
  ExpectEq("line start token", (int)p.buf[0], TEXT_LINE_START_X);
  ExpectEq("line start x", (int)p.buf[1], 3);
  ExpectEq("effective negative margin applied", p.pen.x, 6);
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

} // namespace

int main() {
  TestEmitFlowedLtrWrapsIntoPageBuffer();
  TestEmitFlowedRtlAddsParagraphAndWidthTokens();
  TestEmitKeepsClosingPunctuationOnCurrentLine();
  TestEmitStartsLtrLineAtLeftMargin();
  TestEmitMovesFreshLineFromBaseToEffectiveLeftMargin();
  TestEmitRepeatsLeftMarginTokenAfterWrap();
  TestTransformedTextIsMeasuredBeforeFlowing();
  return 0;
}
