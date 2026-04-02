/*
    3dslibris - epub_toc_diag_utils.h

    TOC normalization and debug logging helpers extracted from epub.cpp.
*/

#pragma once

#include <string>
#include <vector>

#include "book/book.h"
#include "formats/epub/epub_ncx_parser.h"

class IStatusReporter;

namespace epub_toc_diag_utils {

std::string NormalizeTocTitle(const std::string &raw);
std::string NormalizeAsciiSearchText(const std::string &raw,
                                     size_t max_out = 0);
std::string ClipForDiag(const std::string &s, size_t max_bytes = 120);

void LogTocEntrySamples(IStatusReporter *reporter, const char *stage,
                        const std::vector<toc_entry_t> &entries,
                        size_t max_entries = 5);
void LogResolvedChapterSamples(IStatusReporter *reporter, const char *stage,
                               const std::vector<ChapterEntry> &entries,
                               size_t max_entries = 5);
void LogTocResolveDecision(IStatusReporter *reporter, size_t index,
                           const toc_entry_t &src,
                           const std::string &normalized_title,
                           const char *method, bool have_page, u16 page,
                           const char *note = NULL);

} // namespace epub_toc_diag_utils
