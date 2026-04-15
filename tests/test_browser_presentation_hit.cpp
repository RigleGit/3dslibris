#include "library/browser_presentation_hit_utils.h"

#include <cstdlib>
#include <cstdio>
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

}

int main() {
  ExpectEq("grid hit first cell",
           browser_presentation_hit_utils::HitTestGridBookIndex(
               10, 10, 0, 8, 5, 3, 115, 144, 2, 2, 4),
           0);
  ExpectEq("grid hit second column",
           browser_presentation_hit_utils::HitTestGridBookIndex(
               130, 10, 0, 8, 5, 3, 115, 144, 2, 2, 4),
           1);
  ExpectEq("grid hit third item",
           browser_presentation_hit_utils::HitTestGridBookIndex(
               10, 160, 0, 8, 5, 3, 115, 144, 2, 2, 4),
           2);
  ExpectEq("grid miss below cells",
           browser_presentation_hit_utils::HitTestGridBookIndex(
               10, 295, 0, 8, 5, 3, 115, 144, 2, 2, 4),
           -1);

  ExpectEq("list hit first row",
           browser_presentation_hit_utils::HitTestListBookIndex(
               20, 10, 0, 10, 7, 5, 4, 230, 36, 38),
           0);
  ExpectEq("list hit second row",
           browser_presentation_hit_utils::HitTestListBookIndex(
               20, 45, 0, 10, 7, 5, 4, 230, 36, 38),
           1);
  ExpectEq("list hit respects page start",
           browser_presentation_hit_utils::HitTestListBookIndex(
               20, 45, 7, 10, 7, 5, 4, 230, 36, 38),
           8);
  ExpectEq("list miss outside width",
           browser_presentation_hit_utils::HitTestListBookIndex(
               239, 10, 0, 10, 7, 5, 4, 230, 36, 38),
           -1);

  return 0;
}
