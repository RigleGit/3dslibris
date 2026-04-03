/*
    3dslibris - mobi_markup_extract.h

    Extract MOBI markup into plain text with heading/image hints.
*/

#pragma once

#include <3ds/types.h>

#include <string>
#include <utility>
#include <vector>

#include "book/book_parse_deps.h"
#include "formats/mobi/mobi_toc_finalize.h"

class Book;

namespace mobi_markup_extract {

struct ExtractCallbacks {
  std::string (*trim_ascii_whitespace)(const std::string &in);
  std::string (*collapse_ascii_whitespace)(const std::string &in);
  std::string (*fold_latin_for_match)(const std::string &in);
  bool (*looks_like_structured_chapter_title)(const std::string &title);
  void (*add_chapter_at_page_if_unique)(Book *book, u16 page,
                                        const std::string &title, u8 level);
};

std::string ExtractToText(
    Book *book, const BookParseDeps &deps, const std::string &in,
    std::vector<mobi_toc_finalize::MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map,
    const ExtractCallbacks &callbacks);

} // namespace mobi_markup_extract
