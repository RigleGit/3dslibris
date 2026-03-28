#ifndef MOBI_POSITION_MAP_H
#define MOBI_POSITION_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace mobi_position_map {

size_t HtmlSampleIntervalForTextBytes(size_t text_bytes);

bool LooksUsableForToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page);

void RemapHtmlToTextAfterCleanup(
    const std::string &text_before_cleanup,
    const std::string &text_after_cleanup,
    std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map);

} // namespace mobi_position_map

#endif
