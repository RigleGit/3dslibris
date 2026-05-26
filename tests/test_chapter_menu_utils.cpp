#include "menus/chapter_menu_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, uint16_t actual, uint16_t expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected chapter index");
}

} // namespace

int main() {
  std::vector<uint16_t> pages;
  pages.push_back(0);
  pages.push_back(4);
  pages.push_back(10);
  pages.push_back(16);

  ExpectEq("selects first chapter at start",
           chapter_menu_utils::FindChapterIndexForPage(pages, 0), 0);
  ExpectEq("selects containing chapter between starts",
           chapter_menu_utils::FindChapterIndexForPage(pages, 7), 1);
  ExpectEq("selects exact chapter start",
           chapter_menu_utils::FindChapterIndexForPage(pages, 10), 2);
  ExpectEq("selects last chapter after final start",
           chapter_menu_utils::FindChapterIndexForPage(pages, 99), 3);
  ExpectEq("empty list falls back to first row",
           chapter_menu_utils::FindChapterIndexForPage(
               std::vector<uint16_t>(), 12),
           0);

  return 0;
}
