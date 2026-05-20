#include "shared/text_layout_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

int MeasureMono(uint32_t, void *) { return 1; }

void TestShapeAndMeasure() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape text run",
             text_layout_utils::ShapeTextRunUtf8("hola mundo", 10, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("run size", run.size(), (size_t)10);
  ExpectEq("measure all", text_layout_utils::MeasureTextRun(run, 0, run.size()),
           10);
}

void TestShapeSimpleLatinUtf8() {
  const std::string text = std::string("caf\xC3\xA9 ") + "\xE2\x80\x94" +
                           " ma\xC3\xB1"
                           "ana";
  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = true;
  ExpectTrue("shape latin utf8",
             text_layout_utils::ShapeTextRunBidi(text.c_str(), text.size(),
                                                 NULL, MeasureMono, NULL, &run,
                                                 &has_rtl));
  ExpectFalse("latin utf8 is not rtl", has_rtl);
  ExpectEq("latin utf8 codepoints", run.size(), (size_t)13);
  ExpectEq("accented e codepoint", (int)run[3].text.codepoint, 0x00E9);
  ExpectEq("em dash codepoint", (int)run[5].text.codepoint, 0x2014);
  ExpectEq("enye codepoint", (int)run[9].text.codepoint, 0x00F1);
}

void TestFindLineBreaks() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape ascii breaks",
             text_layout_utils::ShapeTextRunUtf8("hola mundo", 10, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("break before ascii space",
           text_layout_utils::FindLineBreak(run, 0, 5), (size_t)4);

  run.clear();
  const std::string nbsp = std::string("hola") + "\xC2\xA0" + "mundo";
  ExpectTrue("shape nbsp breaks",
             text_layout_utils::ShapeTextRunUtf8(nbsp.c_str(), nbsp.size(),
                                                 NULL, MeasureMono, NULL,
                                                 &run));
  ExpectEq("nbsp preserves legacy segment semantics",
           text_layout_utils::FindLineBreak(run, 0, 5), (size_t)4);
}

void TestFindPreformattedBreaks() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape preformatted ascii",
             text_layout_utils::ShapeTextRunUtf8(">>> abcdef", 10, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("preformatted prefers whitespace before splitting a word",
           text_layout_utils::FindPreformattedLineBreak(run, 0, 5), (size_t)4);

  run.clear();
  ExpectTrue("shape preformatted keeps later segment",
             text_layout_utils::ShapeTextRunUtf8("foo bar", 7, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("preformatted second break starts from current point",
           text_layout_utils::FindPreformattedLineBreak(run, 4, 3), (size_t)7);

  run.clear();
  ExpectTrue("shape preformatted long token",
             text_layout_utils::ShapeTextRunUtf8("abcdefgh", 8, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("preformatted still splits long token when no break exists",
           text_layout_utils::FindPreformattedLineBreak(run, 0, 5), (size_t)5);
}

void TestMeasureCombinedBreaks() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape combined normal",
             text_layout_utils::ShapeTextRunUtf8("hola mundo", 10, NULL,
                                                 MeasureMono, NULL, &run));
  text_layout_utils::LineBreakMeasureResult normal =
      text_layout_utils::FindLineBreakAndMeasure(run, 0, 5);
  ExpectEq("combined normal end", normal.end_index, (size_t)4);
  ExpectEq("combined normal width", normal.width, 4);

  run.clear();
  ExpectTrue("shape combined preformatted",
             text_layout_utils::ShapeTextRunUtf8(">>> abcdef", 10, NULL,
                                                 MeasureMono, NULL, &run));
  text_layout_utils::LineBreakMeasureResult pre =
      text_layout_utils::FindPreformattedLineBreakAndMeasure(run, 0, 5);
  ExpectEq("combined preformatted end", pre.end_index, (size_t)4);
  ExpectEq("combined preformatted width", pre.width, 4);
}

void TestPreformattedSegmentEdgeFit() {
  ExpectFalse("preformatted exact right edge fits",
              text_layout_utils::PreformattedSegmentNeedsNewLine(12, 216, 228));
  ExpectTrue("preformatted past right edge wraps",
             text_layout_utils::PreformattedSegmentNeedsNewLine(12, 217, 228));
}

void TestKeepsOpeningPunctuationWithFollowingWord() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  const std::string text = "\xC2\xA1Hola!";
  ExpectTrue("shape opening punctuation",
             text_layout_utils::ShapeTextRunUtf8(text.c_str(), text.size(), NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("opening punctuation does not break alone",
           text_layout_utils::FindLineBreak(run, 0, 2), (size_t)2);
}

void TestKeepsClosingPunctuationWithPreviousWord() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape closing punctuation",
             text_layout_utils::ShapeTextRunUtf8("Hola!", 5, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("closing punctuation stays with previous letter",
           text_layout_utils::FindLineBreak(run, 0, 4), (size_t)3);
}

void TestKeepsQuestionMarkWithPreviousWord() {
  std::vector<text_layout_utils::ShapedGlyph> run;
  ExpectTrue("shape question punctuation",
             text_layout_utils::ShapeTextRunUtf8("estas?", 6, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("question mark stays with previous letter",
           text_layout_utils::FindLineBreak(run, 0, 5), (size_t)4);
}

void TestPrepareDisplayUtf8_RtlDetection() {
  const char *arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"; // "مرحبا"
  std::string display;
  bool is_rtl = false;
  bool transformed = text_layout_utils::PrepareDisplayUtf8(
      arabic, strlen(arabic), &display, &is_rtl);
  ExpectTrue("prepare rtl marks paragraph", is_rtl);
  ExpectTrue("prepare rtl emits display text", !display.empty());
  ExpectTrue("prepare rtl reports transform", transformed);

  const char *latin = "hello";
  display.clear();
  is_rtl = true;
  transformed = text_layout_utils::PrepareDisplayUtf8(
      latin, strlen(latin), &display, &is_rtl);
  ExpectTrue("prepare ltr keeps text", display == "hello");
  ExpectTrue("prepare ltr clears rtl flag", !is_rtl);
  ExpectTrue("prepare ltr no transform", !transformed);
}

} // namespace

int main() {
  TestShapeAndMeasure();
  TestShapeSimpleLatinUtf8();
  TestFindLineBreaks();
  TestFindPreformattedBreaks();
  TestMeasureCombinedBreaks();
  TestPreformattedSegmentEdgeFit();
  TestKeepsOpeningPunctuationWithFollowingWord();
  TestKeepsClosingPunctuationWithPreviousWord();
  TestKeepsQuestionMarkWithPreviousWord();
  TestPrepareDisplayUtf8_RtlDetection();
  return 0;
}
