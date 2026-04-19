#ifndef MOBI_TOC_FINALIZE_POLICY_H
#define MOBI_TOC_FINALIZE_POLICY_H

#include <stdint.h>

#include <utility>
#include <vector>

namespace mobi_toc_finalize_policy {

bool ShouldApplyStructuredToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page);

} // namespace mobi_toc_finalize_policy

#endif
