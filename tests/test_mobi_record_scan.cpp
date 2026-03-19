#include "mobi_record_scan.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, unsigned actual, unsigned expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

} // namespace

int main() {
  using mobi_record_scan::CoverLastResortProbeLimit;
  using mobi_record_scan::FirstImageProbeLimit;

  ExpectEq("small first-image probe scans all remaining records",
           FirstImageProbeLimit(48), 48);
  ExpectEq("large first-image probe is capped",
           FirstImageProbeLimit(600), 128);

  ExpectEq("small last-resort cover scan scans all remaining records",
           CoverLastResortProbeLimit(72), 72);
  ExpectEq("large last-resort cover scan is capped",
           CoverLastResortProbeLimit(1200), 192);
  return 0;
}
