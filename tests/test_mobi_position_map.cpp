#include "formats/mobi/mobi_position_map.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEqU32(const char *label, uint32_t actual, uint32_t expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected integer value");
}

void ExpectEqSize(const char *label, size_t actual, size_t expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected size value");
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void TestSimpleDeletionRemap() {
  std::vector<std::pair<uint32_t, uint32_t>> map;
  map.push_back(std::make_pair(0u, 0u));
  map.push_back(std::make_pair(8u, 8u));
  mobi_position_map::RemapHtmlToTextAfterCleanup("abcXXdef", "abcdef", &map);
  ExpectEqU32("simple last entry", map.back().second, 6u);
}

void TestFallbackWhenExactRemapCollapses() {
  std::vector<std::pair<uint32_t, uint32_t>> map;
  map.push_back(std::make_pair(0u, 0u));
  map.push_back(std::make_pair(6u, 4096u));
  map.push_back(std::make_pair(12u, 8192u));

  std::string before(8192, 'a');
  std::string after(4096, 'b');
  mobi_position_map::RemapHtmlToTextAfterCleanup(before, after, &map);

  ExpectEqU32("fallback last entry", map.back().second, 4096u);
  ExpectTrue("fallback monotonic", map[1].second <= map[2].second);
}

void TestLooksUsableForToc() {
  std::vector<std::pair<uint32_t, uint32_t>> good_map;
  good_map.push_back(std::make_pair(0u, 0u));
  good_map.push_back(std::make_pair(100u, 5000u));
  std::vector<uint32_t> cursors;
  cursors.push_back(0u);
  cursors.push_back(4200u);
  ExpectTrue("good toc map usable",
             mobi_position_map::LooksUsableForToc(good_map, cursors));

  std::vector<std::pair<uint32_t, uint32_t>> bad_map;
  bad_map.push_back(std::make_pair(0u, 0u));
  bad_map.push_back(std::make_pair(100u, 1186u));
  ExpectTrue("bad toc map unusable",
             !mobi_position_map::LooksUsableForToc(bad_map, cursors));
}

void TestAdaptiveSamplingIntervals() {
  ExpectEqSize("small sample interval",
               mobi_position_map::HtmlSampleIntervalForTextBytes(256 * 1024),
               256u);
  ExpectEqSize("threshold sample interval",
               mobi_position_map::HtmlSampleIntervalForTextBytes(1024 * 1024),
               512u);
  ExpectEqSize("large sample interval",
               mobi_position_map::HtmlSampleIntervalForTextBytes(5 * 1024 * 1024),
               512u);
}

} // namespace

int main() {
  TestSimpleDeletionRemap();
  TestFallbackWhenExactRemapCollapses();
  TestLooksUsableForToc();
  TestAdaptiveSamplingIntervals();
  return 0;
}
