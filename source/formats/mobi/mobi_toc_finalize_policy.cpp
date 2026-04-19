#include "formats/mobi/mobi_toc_finalize_policy.h"

#include "formats/mobi/mobi_position_map.h"

namespace mobi_toc_finalize_policy {

bool ShouldApplyStructuredToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page) {
  return mobi_position_map::LooksUsableForToc(html_to_text_map,
                                              text_cursor_per_page);
}

} // namespace mobi_toc_finalize_policy
