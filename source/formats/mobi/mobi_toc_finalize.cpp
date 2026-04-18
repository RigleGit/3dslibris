/*
    3dslibris - mobi_toc_finalize.cpp
*/

#include "formats/mobi/mobi_toc_finalize.h"

#include "book/book.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/mobi/mobi_toc_apply.h"
#include "debug_log.h"
#include "shared/status_reporter.h"

#include <algorithm>

namespace mobi_toc_finalize {

size_t BuildChaptersFromHints(Book *book,
                              const std::vector<MobiHeadingHint> &hints,
                              const FinalizeCallbacks &callbacks) {
  if (!book || hints.empty() || book->GetPageCount() == 0 ||
      !callbacks.normalize_heading_needle || !callbacks.page_has_heading_needle ||
      !callbacks.add_chapter_at_page_if_unique)
    return 0;

  std::vector<std::vector<std::string>> page_lines;
  page_lines.resize(book->GetPageCount());
  for (u16 p = 0; p < book->GetPageCount(); p++)
    page_lines[p] =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(p));

  size_t mapped = 0;
  u16 scan_start = 0;
  const u16 page_count = book->GetPageCount();
  for (size_t i = 0; i < hints.size(); i++) {
    std::string needle = callbacks.normalize_heading_needle(hints[i].title);
    if (needle.size() < 3)
      continue;

    int best = -1;
    for (u16 p = scan_start; p < page_count; p++) {
      if (callbacks.page_has_heading_needle(page_lines[p], needle)) {
        best = (int)p;
        break;
      }
    }
    if (best < 0 && scan_start > 0) {
      for (u16 p = 0; p < scan_start; p++) {
        if (callbacks.page_has_heading_needle(page_lines[p], needle)) {
          best = (int)p;
          break;
        }
      }
    }
    if (best < 0)
      continue;

    callbacks.add_chapter_at_page_if_unique(book, (u16)best, hints[i].title,
                                            hints[i].level);
    mapped++;
    scan_start = (u16)best;
  }
  return mapped;
}

void FinalizePreparedToc(
    Book *book, IStatusReporter *reporter,
    const std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
        &structured_toc,
    bool have_structured_toc, bool structured_from_filepos,
    const std::vector<MobiHeadingHint> &heading_hints, u32 text_len_for_pos,
    const std::vector<std::pair<u32, u32>> &html_to_text_map,
    const std::vector<u32> &text_cursor_per_page,
    const FinalizeCallbacks &callbacks, MobiTocFinalizeResult *out) {
  if (!book)
    return;

  if (out) {
    out->mapped_chapters = 0;
    out->structured_entries = structured_toc.size();
    out->structured_direct = 0;
    out->structured_from_filepos = structured_from_filepos;
  }

  size_t mapped_chapters = 0;
  size_t mapped_structured = 0;
  size_t structured_direct = 0;
  bool structured_used = false;

  if (have_structured_toc) {
    const std::vector<ChapterEntry> fallback = book->GetChapters();
    book->ClearChapters();
    mobi_toc_apply::BuildCallbacks toc_callbacks;
    toc_callbacks.normalize_heading_needle = callbacks.normalize_heading_needle;
    toc_callbacks.page_has_heading_needle = callbacks.page_has_heading_needle;
    toc_callbacks.add_chapter_at_page_if_unique =
        callbacks.add_chapter_at_page_if_unique;
    mapped_structured = mobi_toc_apply::BuildChaptersFromStructuredToc(
        book, structured_toc, text_len_for_pos, &structured_direct,
        !structured_from_filepos, html_to_text_map, text_cursor_per_page,
        toc_callbacks, reporter);
    DBG_LOGF(reporter, "MOBI: toc-step A mapped=%u", (unsigned)mapped_structured);
    if (mapped_structured >= 2) {
      structured_used = true;
      DBG_LOGF(reporter, "MOBI: toc-step B prune");
      if (callbacks.prune_front_matter_toc_cluster)
        callbacks.prune_front_matter_toc_cluster(book, reporter);
      DBG_LOGF(reporter, "MOBI: toc-step C size");
      mapped_structured = book->GetChapters().size();

      u16 direct = (structured_direct > 65535) ? 65535 : (u16)structured_direct;
      u16 unresolved = 0;
      if (structured_toc.size() > mapped_structured) {
        size_t miss = structured_toc.size() - mapped_structured;
        unresolved = (miss > 65535) ? 65535 : (u16)miss;
      }
      TocQuality quality = TOC_QUALITY_MIXED;
      if (unresolved == 0 && mapped_structured > 0 &&
          structured_direct * 100 >= mapped_structured * 85) {
        quality = TOC_QUALITY_STRONG;
      }
      DBG_LOGF(reporter, "MOBI: toc-step D confidence");
      book->SetTocConfidence(quality, direct, 0, unresolved);
      mapped_chapters = mapped_structured;
      if (reporter && structured_from_filepos) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MOBI: filepos TOC mapped=%u direct=%u unresolved=%u",
                 (unsigned)mapped_structured, (unsigned)direct,
                 (unsigned)unresolved);
        DBG_LOG(reporter, msg);
      }
    } else {
      book->ClearChapters();
      for (size_t i = 0; i < fallback.size(); i++) {
        book->AddChapter(fallback[i].page, fallback[i].title,
                         fallback[i].level);
      }
    }
  }

  size_t mapped_hints = 0;
  if (!structured_used && !heading_hints.empty()) {
    const std::vector<ChapterEntry> fallback = book->GetChapters();
    book->ClearChapters();
    mapped_hints = BuildChaptersFromHints(book, heading_hints, callbacks);
    if (mapped_hints < 2) {
      book->ClearChapters();
      for (size_t i = 0; i < fallback.size(); i++) {
        book->AddChapter(fallback[i].page, fallback[i].title,
                         fallback[i].level);
      }
    } else {
      mapped_chapters = mapped_hints;
    }
  }

  if (!structured_used && mapped_hints >= 2) {
    u16 mapped = (mapped_hints > 65535) ? 65535 : (u16)mapped_hints;
    u16 unresolved = 0;
    if (heading_hints.size() > mapped_hints) {
      size_t miss = heading_hints.size() - mapped_hints;
      unresolved = (miss > 65535) ? 65535 : (u16)miss;
    }
    TocQuality quality =
        (unresolved == 0) ? TOC_QUALITY_STRONG : TOC_QUALITY_MIXED;
    book->SetTocConfidence(quality, mapped, 0, unresolved);
  } else if (!structured_used) {
    if (callbacks.prune_front_matter_toc_cluster)
      callbacks.prune_front_matter_toc_cluster(book, reporter);

    MobiChapterQualityStats q;
    if (callbacks.is_heuristic_chapter_set_noisy &&
        callbacks.is_heuristic_chapter_set_noisy(book, &q)) {
      book->ClearChapters();
      book->ClearTocConfidence();
      if (reporter) {
        char msg[224];
        snprintf(
            msg, sizeof(msg),
            "MOBI: TOC heuristic rejected ch=%u uniq=%u early=%u/%u noisy=%u "
            "structured=%u win<=%u",
            (unsigned)q.chapters, (unsigned)q.unique_pages,
            (unsigned)q.early_hits, (unsigned)q.chapters,
            (unsigned)(q.tiny_titles + q.noisy_titles),
            (unsigned)q.structured_titles, (unsigned)q.early_window);
        DBG_LOG(reporter, msg);
      }
    }
  }

  if (out) {
    out->mapped_chapters = mapped_chapters;
    out->structured_entries = structured_toc.size();
    out->structured_direct = structured_direct;
    out->structured_from_filepos = structured_from_filepos;
  }
}

} // namespace mobi_toc_finalize
