/*
    3dslibris - mobi_toc_finalize.h

    Finalize structured/heuristic MOBI TOC application.
*/

#pragma once

#include <3ds/types.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/mobi/mobi_structured_toc_parser.h"

class Book;
struct ChapterEntry;
class IStatusReporter;

namespace mobi_toc_finalize {

struct MobiHeadingHint {
  std::string title;
  u8 level;
};

struct MobiChapterQualityStats {
  size_t chapters;
  size_t unique_pages;
  size_t early_hits;
  size_t tiny_titles;
  size_t noisy_titles;
  size_t structured_titles;
  u16 early_window;
};

struct MobiTocFinalizeResult {
  size_t mapped_chapters;
  size_t structured_entries;
  size_t structured_direct;
  bool structured_from_filepos;
};

struct FinalizeCallbacks {
  std::string (*normalize_heading_needle)(const std::string &text);
  bool (*page_has_heading_needle)(const std::vector<std::string> &lines,
                                  const std::string &needle);
  void (*add_chapter_at_page_if_unique)(Book *book, u16 page,
                                        const std::string &title, u8 level);
  size_t (*prune_front_matter_toc_cluster)(Book *book,
                                           IStatusReporter *reporter);
  bool (*is_heuristic_chapter_set_noisy)(Book *book,
                                         MobiChapterQualityStats *stats_out);
};

size_t BuildChaptersFromHints(Book *book,
                              const std::vector<MobiHeadingHint> &hints,
                              const FinalizeCallbacks &callbacks);

void FinalizePreparedToc(
    Book *book, IStatusReporter *reporter,
    const std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
        &structured_toc,
    bool have_structured_toc, bool structured_from_filepos,
    const std::vector<MobiHeadingHint> &heading_hints, u32 text_len_for_pos,
    const std::vector<std::pair<u32, u32>> &html_to_text_map,
    const std::vector<u32> &text_cursor_per_page,
    const FinalizeCallbacks &callbacks, MobiTocFinalizeResult *out,
    size_t applied_mapped = static_cast<size_t>(-1), size_t applied_direct = 0,
    const std::vector<ChapterEntry> *fallback_chapters = nullptr);

} // namespace mobi_toc_finalize
