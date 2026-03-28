#include "shared/text_layout_utils.h"

#include <cstdio>
#include <cstdlib>
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
  ExpectEq("preformatted wraps at width without waiting for whitespace",
           text_layout_utils::FindPreformattedLineBreak(run, 0, 5), (size_t)5);

  run.clear();
  ExpectTrue("shape preformatted keeps later segment",
             text_layout_utils::ShapeTextRunUtf8("foo bar", 7, NULL,
                                                 MeasureMono, NULL, &run));
  ExpectEq("preformatted second break starts from current point",
           text_layout_utils::FindPreformattedLineBreak(run, 4, 3), (size_t)7);
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
  ExpectEq("combined preformatted end", pre.end_index, (size_t)5);
  ExpectEq("combined preformatted width", pre.width, 5);
}

} // namespace

int main() {
  TestShapeAndMeasure();
  TestFindLineBreaks();
  TestFindPreformattedBreaks();
  TestMeasureCombinedBreaks();
  return 0;
}
