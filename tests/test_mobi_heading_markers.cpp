#include "mobi_heading_markers.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

} // namespace

int main() {
  ExpectEq("h1 marker decodes",
           mobi_heading_markers::HeadingLevelFromMarker(
               mobi_heading_markers::MarkerForHeadingLevel(1)),
           1);
  ExpectEq("h2 marker decodes",
           mobi_heading_markers::HeadingLevelFromMarker(
               mobi_heading_markers::MarkerForHeadingLevel(2)),
           2);
  ExpectEq("h3 marker decodes",
           mobi_heading_markers::HeadingLevelFromMarker(
               mobi_heading_markers::MarkerForHeadingLevel(3)),
           3);
  ExpectEq("other control byte ignored",
           mobi_heading_markers::HeadingLevelFromMarker(6), 0);
  return 0;
}
