#include "book_error.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectString(const char *label, const char *actual, const char *expected) {
  std::string lhs = actual ? actual : "";
  std::string rhs = expected ? expected : "";
  if (lhs != rhs)
    Fail(std::string(label) + ": unexpected error text");
}

void ExpectNull(const char *label, const char *value) {
  if (value)
    Fail(std::string(label) + ": expected null");
}

} // namespace

int main() {
  ExpectString("corrupt books have short tag",
               BookOpenErrorTag(BOOK_ERR_CORRUPT), "corrupt_or_empty_book");
  ExpectString("corrupt books have friendly text",
               DescribeBookOpenError(BOOK_ERR_CORRUPT),
               "error: corrupt or empty book");
  ExpectNull("unknown errors have no short tag", BookOpenErrorTag(253));
  ExpectNull("unknown errors fall back to numeric formatting",
             DescribeBookOpenError(253));
  return 0;
}
