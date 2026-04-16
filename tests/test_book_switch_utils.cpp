#include "reader/book_switch_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  fprintf(stderr, "%s\n", msg.c_str());
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

void ExpectEq(const char *label, unsigned int actual, unsigned int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestNextBookSessionId() {
  ExpectEq("next after 1", NextBookSessionId(1), 2);
  ExpectEq("wrap skips zero", NextBookSessionId(0xFFFFFFFFu), 1);
}

void TestShouldCloseCurrentBookForSwitch() {
  int a = 1;
  int b = 2;
  ExpectFalse("null current", ShouldCloseCurrentBookForSwitch(nullptr, &a));
  ExpectFalse("same book", ShouldCloseCurrentBookForSwitch(&a, &a));
  ExpectTrue("different books", ShouldCloseCurrentBookForSwitch(&a, &b));
}

} // namespace

int main() {
  TestNextBookSessionId();
  TestShouldCloseCurrentBookForSwitch();
  return 0;
}
