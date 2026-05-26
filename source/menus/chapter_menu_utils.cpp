#include "menus/chapter_menu_utils.h"

#include <stddef.h>

namespace chapter_menu_utils {

uint16_t FindChapterIndexForPage(const std::vector<uint16_t> &chapter_pages,
                                 uint16_t current_page) {
  uint16_t selected = 0;
  for (size_t i = 0; i < chapter_pages.size(); i++) {
    if (chapter_pages[i] > current_page)
      break;
    selected = (uint16_t)i;
  }
  return selected;
}

} // namespace chapter_menu_utils
