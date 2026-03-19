#include "buffered_status_log.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectStr(const char *label, const std::string &actual,
               const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": unexpected string");
  }
}

} // namespace

int main() {
  buffered_status_log::BufferedStatusLog log(12);
  log.Append("first");
  log.Append("second");
  log.Append("third");

  std::vector<std::string> batches;
  log.Flush([&](const std::string &chunk) { batches.push_back(chunk); });

  ExpectEq("flush chunks count", batches.size(), 2);
  ExpectStr("chunk[0]", batches[0], "first\nsecond");
  ExpectStr("chunk[1]", batches[1], "third");

  batches.clear();
  log.Flush([&](const std::string &chunk) { batches.push_back(chunk); });
  ExpectEq("second flush is empty", batches.size(), 0);

  return 0;
}
