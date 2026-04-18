/*
    3dslibris - mobi_toc_apply.h

    Apply structured MOBI TOC entries to chapter pages.
*/

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <3ds/types.h>

#include "formats/mobi/mobi_structured_toc_parser.h"

class Book;
class IStatusReporter;

namespace mobi_toc_apply {

struct BuildCallbacks {
  std::string (*normalize_heading_needle)(const std::string &text);
  bool (*page_has_heading_needle)(const std::vector<std::string> &lines,
                                  const std::string &needle);
  void (*add_chapter_at_page_if_unique)(Book *book, u16 page,
                                        const std::string &title, u8 level);
};

int HtmlPosToPage(u32 html_pos,
                  const std::vector<std::pair<u32, u32>> &html_to_text_map,
                  const std::vector<u32> &text_cursor_per_page,
                  u32 *text_pos_out = nullptr);

__attribute__((noinline)) size_t BuildChaptersFromStructuredToc(
    Book *book,
    const std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> &entries,
    u32 text_len, size_t *direct_out, bool refine_with_heading_search,
    const std::vector<std::pair<u32, u32>> &html_to_text_map,
    const std::vector<u32> &text_cursor_per_page,
    const BuildCallbacks &callbacks, IStatusReporter *reporter);

} // namespace mobi_toc_apply
