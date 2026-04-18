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

void ExpectCString(const char *label, const char *actual, const char *expected) {
  if (std::string(actual ? actual : "") != std::string(expected)) {
    Fail(std::string(label) + ": expected " + expected + ", got " +
         (actual ? actual : "(null)"));
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

void TestShouldAttachOpeningResult() {
  ExpectTrue("valid attach", ShouldAttachOpeningResult(7, 7, false, 4));
  ExpectFalse("stale attach blocked",
              ShouldAttachOpeningResult(0, 7, false, 4));
  ExpectFalse("session mismatch blocked",
              ShouldAttachOpeningResult(7, 8, false, 4));
  ExpectFalse("aborted blocked",
              ShouldAttachOpeningResult(7, 7, true, 4));
  ExpectFalse("empty pages blocked",
              ShouldAttachOpeningResult(7, 7, false, 0));
}

void TestDescribeOpeningFailureCause() {
  ExpectCString("stale cause",
                DescribeOpeningFailureCause(0, 7, false, 4),
                "stale-session");
  ExpectCString("session cause",
                DescribeOpeningFailureCause(7, 8, false, 4),
                "session-mismatch");
  ExpectCString("aborted cause",
                DescribeOpeningFailureCause(7, 7, true, 4),
                "aborted");
  ExpectCString("empty cause",
                DescribeOpeningFailureCause(7, 7, false, 0),
                "empty-parse");
}

} // namespace

int main() {
  TestNextBookSessionId();
  TestShouldCloseCurrentBookForSwitch();
  TestShouldAttachOpeningResult();
  TestDescribeOpeningFailureCause();
  return 0;
}
