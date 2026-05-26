#pragma once

#include <stdint.h>
#include <vector>

namespace chapter_menu_utils {

uint16_t FindChapterIndexForPage(const std::vector<uint16_t> &chapter_pages,
                                 uint16_t current_page);

} // namespace chapter_menu_utils
