/*
    3dslibris - epub_toc_title_match_utils.h

    TOC title-to-page matching helpers extracted from epub.cpp.
*/

#pragma once

#include <string>
#include <vector>

#include "book/book.h"

namespace epub_toc_title_match_utils {

bool PathLooksLikeTocDocForFallback(const std::string &path);

bool FindTocTitlePageInDocRange(Book *book, u16 doc_start,
                                const std::vector<u16> &doc_starts,
                                const std::string &toc_title, u16 *page_out);

bool FindTocTitlePageGlobal(Book *book, const std::string &toc_title,
                            u16 from_page, u16 *page_out,
                            bool allow_wrap = true,
                            u16 to_page_exclusive = 0);

bool FindChapterPageFromParsedHeadings(
    const std::vector<ChapterEntry> &chapters, const std::string &toc_title,
    u16 min_page, u16 *page_out);

} // namespace epub_toc_title_match_utils

