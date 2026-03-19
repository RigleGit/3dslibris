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

} // namespace

int main() {
  TestShapeAndMeasure();
  TestFindLineBreaks();
  return 0;
}
