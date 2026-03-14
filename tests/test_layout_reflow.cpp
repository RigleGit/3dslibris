#include "layout_reflow.h"

#include <cstdlib>
#include <initializer_list>
#include <list>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEqualInt(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectListEq(const char *label, const std::list<int> &actual,
                  std::initializer_list<int> expected) {
  std::list<int> expected_list(expected);
  if (actual != expected_list) {
    std::string message = std::string(label) + ": unexpected bookmark remap";
    Fail(message);
  }
}

} // namespace

int main() {
  using layout_reflow::RemapBookmarksApprox;
  using layout_reflow::RemapPageIndexApprox;

  ExpectEqualInt("keeps first page anchored", RemapPageIndexApprox(0, 10, 20),
                 0);
  ExpectEqualInt("keeps last page anchored", RemapPageIndexApprox(9, 10, 20),
                 19);
  ExpectEqualInt("maps middle page proportionally",
                 RemapPageIndexApprox(5, 10, 20), 11);
  ExpectEqualInt("clamps empty old counts", RemapPageIndexApprox(12, 0, 7), 6);
  ExpectEqualInt("clamps empty new counts", RemapPageIndexApprox(3, 10, 0), 0);

  ExpectListEq("deduplicates and sorts bookmarks",
               RemapBookmarksApprox({9, 5, 5, 0}, 10, 20), {0, 11, 19});
  ExpectListEq("clamps bookmark remap into new range",
               RemapBookmarksApprox({12}, 10, 7), {6});

  return 0;
}
