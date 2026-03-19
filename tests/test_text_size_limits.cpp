#include "ui/text_limits.h"

#include <cstdio>
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
  ExpectEq("clamp low", ClampTextPixelSize(0), 8);
  ExpectEq("clamp low edge", ClampTextPixelSize(7), 8);
  ExpectEq("keep min", ClampTextPixelSize(8), 8);
  ExpectEq("keep middle", ClampTextPixelSize(12), 12);
  ExpectEq("keep max", ClampTextPixelSize(20), 20);
  ExpectEq("clamp high", ClampTextPixelSize(21), 20);
  ExpectEq("clamp high large", ClampTextPixelSize(255), 20);
  return 0;
}
