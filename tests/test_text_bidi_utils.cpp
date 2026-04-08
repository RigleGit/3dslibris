#include "shared/text_bidi_utils.h"

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

void ExpectEqInt(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectEqU32(const char *label, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected 0x%04X, got 0x%04X",
             label, expected, actual);
    Fail(buf);
  }
}

void TestContainsRTL_LatinOnly() {
  const uint32_t latin[] = {'H', 'e', 'l', 'l', 'o'};
  ExpectFalse("Latin-only ContainsRTL",
              text_bidi_utils::ContainsRTL(latin, 5));
}

void TestContainsRTL_HebrewOnly() {
  const uint32_t hebrew[] = {0x05E9, 0x05DC, 0x05D5, 0x05DD};
  ExpectTrue("Hebrew ContainsRTL",
             text_bidi_utils::ContainsRTL(hebrew, 4));
}

void TestContainsRTL_Mixed() {
  const uint32_t mixed[] = {'H', 'e', 'l', 'l', 'o', ' ',
                             0x05E9, 0x05DC, 0x05D5, 0x05DD, ' ',
                             'W', 'o', 'r', 'l', 'd'};
  ExpectTrue("Mixed ContainsRTL",
             text_bidi_utils::ContainsRTL(mixed, 16));
}

void TestContainsRTL_Empty() {
  ExpectFalse("Empty ContainsRTL",
              text_bidi_utils::ContainsRTL(NULL, 0));
}

void TestAnalyzeBidiRuns_LatinOnly() {
  const uint32_t latin[] = {'H', 'e', 'l', 'l', 'o'};
  std::vector<text_bidi_utils::BidiRun> runs;
  bool ok = text_bidi_utils::AnalyzeBidiRuns(latin, 5, &runs);
  ExpectTrue("AnalyzeBidiRuns Latin ok", ok);
  ExpectEq("Latin single run", runs.size(), (size_t)1);
  ExpectEq("Latin run start", runs[0].start, (size_t)0);
  ExpectEq("Latin run length", runs[0].length, (size_t)5);
  ExpectEqInt("Latin run level LTR", runs[0].bidi_level & 1, 0);
}

void TestAnalyzeBidiRuns_HebrewOnly() {
  const uint32_t hebrew[] = {0x05D0, 0x05D1, 0x05D2, 0x05D3};
  std::vector<text_bidi_utils::BidiRun> runs;
  bool ok = text_bidi_utils::AnalyzeBidiRuns(hebrew, 4, &runs);
  ExpectTrue("AnalyzeBidiRuns Hebrew ok", ok);
  ExpectEq("Hebrew single run", runs.size(), (size_t)1);
  ExpectEq("Hebrew run start", runs[0].start, (size_t)0);
  ExpectEq("Hebrew run length", runs[0].length, (size_t)4);
  ExpectEqInt("Hebrew run level RTL", runs[0].bidi_level & 1, 1);
}

void TestAnalyzeBidiRuns_Mixed() {
  const uint32_t mixed[] = {'H', 'e', 'l', 'l', 'o', ' ',
                             0x05E9, 0x05DC, 0x05D5, 0x05DD, ' ',
                             'W', 'o', 'r', 'l', 'd'};
  std::vector<text_bidi_utils::BidiRun> runs;
  bool ok = text_bidi_utils::AnalyzeBidiRuns(mixed, 16, &runs);
  ExpectTrue("AnalyzeBidiRuns Mixed ok", ok);
  ExpectTrue("Mixed has 3+ runs", runs.size() >= 3);

  ExpectEqInt("First run LTR", runs[0].bidi_level & 1, 0);

  bool found_rtl = false;
  for (size_t i = 0; i < runs.size(); i++) {
    if (runs[i].bidi_level & 1)
      found_rtl = true;
  }
  ExpectTrue("Mixed has RTL run", found_rtl);
}

void TestReorderLineForDisplay_NoRTL() {
  std::vector<uint32_t> cps = {'H', 'e', 'l', 'l', 'o'};
  std::vector<text_bidi_utils::BidiRun> runs;
  text_bidi_utils::BidiRun run;
  run.start = 0;
  run.length = 5;
  run.bidi_level = 0;
  runs.push_back(run);

  std::vector<uint32_t> expected = cps;
  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), runs);
  for (size_t i = 0; i < cps.size(); i++)
    ExpectEqU32("LTR no reorder", cps[i], expected[i]);
}

void TestReorderLineForDisplay_RTLSegment() {
  std::vector<uint32_t> cps = {0x05D0, 0x05D1, 0x05D2};
  std::vector<text_bidi_utils::BidiRun> runs;
  text_bidi_utils::BidiRun run;
  run.start = 0;
  run.length = 3;
  run.bidi_level = 1;
  runs.push_back(run);

  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), runs);
  ExpectEqU32("RTL reversed [0]", cps[0], 0x05D2);
  ExpectEqU32("RTL reversed [1]", cps[1], 0x05D1);
  ExpectEqU32("RTL reversed [2]", cps[2], 0x05D0);
}

void TestReorderLineForDisplay_MixedLine() {
  std::vector<uint32_t> cps = {'A', 'B', 0x05D0, 0x05D1, 'C', 'D'};
  std::vector<text_bidi_utils::BidiRun> runs;

  text_bidi_utils::BidiRun ltr1;
  ltr1.start = 0;
  ltr1.length = 2;
  ltr1.bidi_level = 0;
  runs.push_back(ltr1);

  text_bidi_utils::BidiRun rtl;
  rtl.start = 2;
  rtl.length = 2;
  rtl.bidi_level = 1;
  runs.push_back(rtl);

  text_bidi_utils::BidiRun ltr2;
  ltr2.start = 4;
  ltr2.length = 2;
  ltr2.bidi_level = 0;
  runs.push_back(ltr2);

  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), runs);

  ExpectEqU32("Mixed reorder [0] A", cps[0], 'A');
  ExpectEqU32("Mixed reorder [1] B", cps[1], 'B');
  ExpectEqU32("Mixed reorder [2] reversed", cps[2], 0x05D1);
  ExpectEqU32("Mixed reorder [3] reversed", cps[3], 0x05D0);
  ExpectEqU32("Mixed reorder [4] C", cps[4], 'C');
  ExpectEqU32("Mixed reorder [5] D", cps[5], 'D');
}

void TestReorderLineForDisplay_RtlWithNumbers() {
  // Visual expectation: Arabic run reversed, ASCII digits keep internal order.
  std::vector<uint32_t> cps = {0x0633, 0x0644, 0x0627, 0x0645, ' ', '1', '2',
                               '3'};
  std::vector<text_bidi_utils::BidiRun> runs;

  text_bidi_utils::BidiRun rtl;
  rtl.start = 0;
  rtl.length = 5;
  rtl.bidi_level = 1;
  runs.push_back(rtl);

  text_bidi_utils::BidiRun ltr_num;
  ltr_num.start = 5;
  ltr_num.length = 3;
  ltr_num.bidi_level = 2;
  runs.push_back(ltr_num);

  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), runs);
  ExpectEqU32("RTL+num [0]", cps[0], '1');
  ExpectEqU32("RTL+num [1]", cps[1], '2');
  ExpectEqU32("RTL+num [2]", cps[2], '3');
  ExpectEqU32("RTL+num [3]", cps[3], ' ');
  ExpectEqU32("RTL+num [4]", cps[4], 0x0645);
}

} // namespace

int main() {
  TestContainsRTL_LatinOnly();
  TestContainsRTL_HebrewOnly();
  TestContainsRTL_Mixed();
  TestContainsRTL_Empty();
  TestAnalyzeBidiRuns_LatinOnly();
  TestAnalyzeBidiRuns_HebrewOnly();
  TestAnalyzeBidiRuns_Mixed();
  TestReorderLineForDisplay_NoRTL();
  TestReorderLineForDisplay_RTLSegment();
  TestReorderLineForDisplay_MixedLine();
  TestReorderLineForDisplay_RtlWithNumbers();
  printf("All text_bidi_utils tests passed.\n");
  return 0;
}
