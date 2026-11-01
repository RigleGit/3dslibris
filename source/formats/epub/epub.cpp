/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2008 Ray Haleblian

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

/*
  3DS port modifications by Rigle (summary):
  - Hardened TOC/NAV parsing paths and bounded memory/entry limits.
  - Added diagnostic timing/logging and on-demand TOC resolution flow.
  - Improved cover discovery and resilient chapter mapping fallbacks.
*/

#include "formats/epub/epub.h"
#include "formats/epub/epub_preparation.h"
#include "formats/epub/epub_block_cache.h"
#include "formats/common/page_cache_utils.h"
#include "shared/path_constants.h"
#include <new>
#include <memory>

#include "formats/epub/epub_cache.h"
#include "formats/epub/epub_manifest.h"
#include "formats/epub/epub_toc.h"

#include "shared/base64_utils.h"
#include "book/book.h"
#include "book/book_parse_deps.h"
#include "book/book_xml.h"
#include "shared/debug_log.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/book_error.h"
#include "formats/common/xml_parse_utils.h"
#include "formats/epub/epub_page_cache.h"
#include "settings/prefs.h"
#include "formats/epub/epub_cover.h"
#include "formats/epub/epub_limits.h"
#include "formats/epub/epub_ncx_parser.h"
#include "formats/epub/epub_package_toc_utils.h"
#include "formats/epub/epub_toc_diag_utils.h"
#include "formats/epub/epub_toc_package_loader_utils.h"
#include "formats/epub/epub_toc_title_match_utils.h"
#include "formats/epub/epub_zip_utils.h"
#include "shared/path_utils.h"
#include "book/page.h"
#include "shared/parser_limits.h"
#include "shared/open_cancel_poll.h"
#include "shared/status_reporter.h"
#include "shared/text_layout_utils.h"

#ifdef DSLIBRIS_DEBUG
// Process-wide aggregator defined in epub_manifest.cpp. Forward-declared
// here to keep the per-doc accumulator private to that translation unit
// while still letting the spine-level summary read it.
namespace epub_parse_perf {
struct Snapshot {
  u64 chardata_ms;
  u64 element_ms;
  u64 flush_ms;
  u32 chardata_calls;
  u32 element_calls;
  u32 flush_calls;
  u32 page_overflows;
};
Snapshot Get();
} // namespace epub_parse_perf
#endif
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "stb_image.h"
#include "shared/string_utils.h"
#include "minizip/unzip.h"
#include "zlib.h"
#include <3ds.h>
#include <algorithm>
#include <ctype.h>
#include <deque>
#include <limits.h>
#include <map>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#ifndef EPUB_METADATA_TRACE
#define EPUB_METADATA_TRACE 0
#endif

typedef BookParseDeps EpubDeps;

static EpubDeps BuildEpubDeps(Book *book) { return BuildBookParseDeps(book); }

static bool ShouldAbortEpubOpen(Book *book) {
  return book &&
         ((book->GetStatusReporter() &&
           book->GetStatusReporter()->ShouldAbortWork()) ||
          book->IsOpenAbortRequested());
}

#ifdef DSLIBRIS_DEBUG
static u64 SafeElapsedMs(u64 later, u64 earlier) {
  return later >= earlier ? (later - earlier) : 0;
}
#endif

using epub_toc_diag_utils::ClipForDiag;
using epub_toc_diag_utils::LogResolvedChapterSamples;
using epub_toc_diag_utils::LogTocEntrySamples;
using epub_toc_diag_utils::LogTocResolveDecision;
using epub_toc_diag_utils::NormalizeAsciiSearchText;
using epub_toc_diag_utils::NormalizeTocTitle;
using epub_package_toc_utils::BuildDocPath;
using epub_package_toc_utils::ExtractHrefFragment;
using epub_package_toc_utils::FindManifestItemPath;
using epub_package_toc_utils::LocateZipEntrySafe;
using epub_package_toc_utils::LookupTocProxyHref;
using epub_package_toc_utils::NormalizeFragmentId;
using epub_package_toc_utils::ReadZipEntryText;
using epub_toc_package_loader_utils::BuildPageStartMapFromPackage;
using epub_toc_package_loader_utils::LoadTocEntriesFromPackage;
using epub_toc_title_match_utils::FindChapterPageFromParsedHeadings;
using epub_toc_title_match_utils::FindTocTitlePageGlobal;
using epub_toc_title_match_utils::FindTocTitlePageInDocRange;
using epub_toc_title_match_utils::PathLooksLikeTocDocForFallback;

static std::string BuildChapterLabel(const std::string &path, int chapter_num) {
  std::string base = path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos)
    base = base.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos)
    base = base.substr(0, dot);

  for (size_t i = 0; i < base.size(); i++) {
    if (base[i] == '_' || base[i] == '-')
      base[i] = ' ';
  }

  // Normalize consecutive spaces.
  std::string normalized;
  normalized.reserve(base.size());
  bool prev_space = true;
  for (size_t i = 0; i < base.size(); i++) {
    char c = base[i];
    bool is_space = (c == ' ');
    if (is_space && prev_space)
      continue;
    normalized.push_back(c);
    prev_space = is_space;
  }
  while (!normalized.empty() && normalized.back() == ' ')
    normalized.pop_back();

  if (normalized.empty()) {
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "chapter %d", chapter_num);
    normalized = fallback;
  }

  char label[160];
  snprintf(label, sizeof(label), "%02d - %s", chapter_num, normalized.c_str());
  return std::string(label);
}

static std::string BuildChapterLabelFromText(const std::string &raw_title,
                                             int chapter_num) {
  std::string title = Trim(raw_title);
  if (title.empty())
    return "";

  std::string normalized;
  normalized.reserve(title.size());
  bool prev_space = true;
  for (size_t i = 0; i < title.size(); i++) {
    unsigned char uc = (unsigned char)title[i];
    bool is_space =
        isspace(uc) || title[i] == '\n' || title[i] == '\r' || title[i] == '\t';
    if (is_space) {
      if (!prev_space)
        normalized.push_back(' ');
      prev_space = true;
      continue;
    }
    normalized.push_back((char)uc);
    prev_space = false;
    if (normalized.size() >= 96)
      break;
  }

  normalized = Trim(normalized);
  if (normalized.empty())
    return "";

  char label[192];
  snprintf(label, sizeof(label), "%02d - %s", chapter_num, normalized.c_str());
  return std::string(label);
}


static int OpenEpubArchive(const std::string &name, unzFile *uf) {
  if (!uf)
    return 1;
  *uf = unzOpen(name.c_str());
  return *uf ? 0 : 1;
}

namespace {
const int kSpinePending = -32760;
struct SpineCursor {
  epub_zip_utils::ZipEntryIndex zip_index;
  size_t next, document;
  int chapter;
  bool initialized, done;
  unzFile inline_zip, css_zip;
#ifdef DSLIBRIS_DEBUG
  u64 started;
  text_layout_utils::PerfStats layout_before;
  epub_parse_perf::Snapshot parse_before;
#endif
  SpineCursor()
      : next(0), document(0), chapter(1), initialized(false), done(false),
        inline_zip(NULL), css_zip(NULL) {}
  ~SpineCursor() {
    if (inline_zip)
      unzClose(inline_zip);
    if (css_zip)
      unzClose(css_zip);
  }
};
} // namespace

static int ParseEpubSpineDocuments(
    unzFile uf, Book *book, epub_data_t *parsedata,
    const std::string &archive_path, const std::string &folder,
    const std::vector<std::string> &hrefs, const EpubDeps &deps,
    std::map<std::string, u16> *page_start_by_href,
    epub_page_cache::StreamWriter *stream_writer, SpineCursor *resume
#ifdef DSLIBRIS_DEBUG
    ,
    u64 *t_after_content
#endif
) {
  if (!uf || !book || !parsedata || !page_start_by_href)
    return 1;

  IStatusReporter *reporter = deps.reporter;
  SpineCursor local;
  SpineCursor &work = resume ? *resume : local;
  epub_zip_utils::ZipEntryIndex &zip_index = work.zip_index;
  int rc = 0;
  if (!work.initialized) {
    for (auto *ctx : parsedata->ctx)
      delete ctx;
    parsedata->ctx.clear();
    parsedata->book = book;
    parsedata->type = PARSE_CONTENT;
    book->ClearChapters();
    book->ClearChapterDocStartPages();
    book->ClearInlineImages();
    page_start_by_href->clear();
    work.inline_zip =
        book->GetEbookImagesEnabled() ? unzOpen(archive_path.c_str()) : NULL;
    work.css_zip = unzOpen(archive_path.c_str());
    work.initialized = true;
#ifdef DSLIBRIS_DEBUG
    work.started = osGetTime();
    work.layout_before = text_layout_utils::GetPerfStats();
    work.parse_before = epub_parse_perf::Get();
#endif
  }
  book->SetInlineImageProbeZip(work.inline_zip);
  unzFile css_scan_uf = work.css_zip;
  int &chapter_num = work.chapter;
  size_t &spine_doc_index = work.document;
#ifdef DSLIBRIS_DEBUG
  const u64 t_spine_begin = work.started;
  const text_layout_utils::PerfStats &spine_layout_before = work.layout_before;
  const epub_parse_perf::Snapshot &spine_parse_before = work.parse_before;
#endif
  const size_t initial = work.next;
  for (size_t i = work.next; i < hrefs.size(); i++) {
    if (resume && i != initial)
      return kSpinePending;
    work.next = i + 1;
    if (open_cancel_poll::Poll(book, reporter, "epub-spine-loop")) {
      rc = BOOK_ERR_CANCELLED;
      break;
    }
    if (book->GetPageCount() >= epub_limits::kMaxPagesInMemory) {
      if (reporter) {
        DBG_LOGF(reporter,
                 "EPUB: page limit reached pages=%u limit=%u spine=%u/%u",
                 (unsigned)book->GetPageCount(),
                 (unsigned)epub_limits::kMaxPagesInMemory,
                 (unsigned)spine_doc_index, (unsigned)hrefs.size());
      }
      break;
    }
    spine_doc_index++;
    const std::string &href = hrefs[i];
    size_t pos = href.find_last_of('.');
    if (pos >= href.length())
      continue;

    std::string path = BuildDocPath(folder, href.c_str());
    std::string path_key = NormalizePath(path);
    if (LocateZipEntrySafe(uf, path, reporter, "SPINE", &zip_index)) {
      u16 chapter_start_page = book->GetPageCount();
      std::string chapter_label = BuildChapterLabel(path, chapter_num);
#ifdef DSLIBRIS_DEBUG
      u64 t_doc_begin = osGetTime();
      size_t anchors_before = book->GetChapterAnchorCount();
#endif
      rc = unzOpenCurrentFile(uf);
      if (rc != UNZ_OK)
        break;
      parsedata->docpath = path;
      parsedata->archive_path = archive_path;
      const int parse_rc = epub_parse_currentfile(
          uf, parsedata, deps, css_scan_uf, &zip_index);
      const int close_rc = unzCloseCurrentFile(uf);
      // Expat XML parse errors (positive, < BOOK_ERR_CANCELLED) are
      // recoverable: real-world EPUBs routinely have malformed XHTML (mismatched
      // tags, bare ampersands, unclosed void elements). Skip the bad document
      // and continue with the rest of the spine rather than failing the whole
      // book. Propagate fatal conditions: cancellation, allocation failure,
      // and ZIP-level errors from minizip (negative codes).
      if (parse_rc == BOOK_ERR_CANCELLED || parse_rc == XML_ERROR_NO_MEMORY) {
        rc = parse_rc;
        break;
      }
      if (parse_rc > 0) {
        if (reporter) {
          DBG_LOGF(reporter, "EPUB: spine doc xml-err=%d path=%s (skipped)",
                   parse_rc, path.c_str());
      }
        rc = 0;
        // Even on recoverable XML errors the parser may have produced pages
        // before hitting the error. Register those pages so that TOC resolution
        // can map NCX hrefs to page numbers.
        if (book->GetPageCount() > chapter_start_page) {
          std::string parsed_title =
              BuildChapterLabelFromText(parsedata->parsed_doc_title, chapter_num);
          if (!parsed_title.empty())
            chapter_label = parsed_title;
          chapter_num++;
          book->AddChapter(chapter_start_page, chapter_label);
          book->SetChapterDocStartPage(path_key, chapter_start_page);
          if (page_start_by_href->find(path_key) == page_start_by_href->end())
            (*page_start_by_href)[path_key] = chapter_start_page;
          if (stream_writer && stream_writer->IsOpen()) {
            if (!stream_writer->FlushPages(book, chapter_start_page)) {
              rc = BOOK_ERR_CANCELLED;
              break;
            }
          }
        }
        continue;
      }
      rc = (parse_rc != 0) ? parse_rc : close_rc;
      if (rc != 0)
        break;
      std::string parsed_title =
          BuildChapterLabelFromText(parsedata->parsed_doc_title, chapter_num);
      if (!parsed_title.empty())
        chapter_label = parsed_title;
      chapter_num++;
      if (book->GetPageCount() > 0) {
        book->AddChapter(chapter_start_page, chapter_label);
        book->SetChapterDocStartPage(path_key, chapter_start_page);
        if (page_start_by_href->find(path_key) == page_start_by_href->end())
          (*page_start_by_href)[path_key] = chapter_start_page;
      }
      if (stream_writer && stream_writer->IsOpen()) {
        if (!stream_writer->FlushPages(book, chapter_start_page)) {
          rc = BOOK_ERR_CANCELLED;
          break;
        }
      }
#ifdef DSLIBRIS_DEBUG
      if (reporter && (spine_doc_index % 20 == 0 || spine_doc_index == 1)) {
        DBG_LOGF(reporter, "EPUB: spine progress %u/%u pages=%u mem_free=%u",
                 (unsigned)spine_doc_index, (unsigned)hrefs.size(),
                 (unsigned)book->GetPageCount(),
                 (unsigned)osGetMemRegionFree(MEMREGION_ALL));
        }
#endif
        if (!resume && (spine_doc_index % 20 == 0 || spine_doc_index == 1))
          book->NotifySpineProgress((unsigned)spine_doc_index,
                                    (unsigned)hrefs.size());
#ifdef DSLIBRIS_DEBUG
      if (reporter) {
        u64 doc_ms = osGetTime() - t_doc_begin;
        unsigned int pages_added =
            (unsigned int)(book->GetPageCount() - chapter_start_page);
        unsigned int anchors_added =
            (unsigned int)(book->GetChapterAnchorCount() - anchors_before);
        if (doc_ms >= 200 || pages_added >= 40 || anchors_added >= 100) {
          DBG_LOGF(reporter,
                   "EPUB: spine doc %u/%u ms=%llums pages=%u anchors=%u path=%s",
                   (unsigned)spine_doc_index, (unsigned)hrefs.size(),
                   (unsigned long long)doc_ms, pages_added, anchors_added,
                   path.c_str());
        }
      }
#endif
      if (open_cancel_poll::Poll(book, reporter, "epub-spine-doc-done")) {
        rc = BOOK_ERR_CANCELLED;
        break;
      }
    } else if (reporter) {
      char msg[256];
      snprintf(msg, sizeof(msg), "NOT FOUND IN ZIP: %s", path.c_str());
      DBG_LOG(reporter, msg);
  }
  }

  work.done = true;
  book->SetInlineImageProbeZip(NULL);
  if (work.inline_zip) {
    unzClose(work.inline_zip);
    work.inline_zip = NULL;
  }
  if (work.css_zip) {
    unzClose(work.css_zip);
    work.css_zip = NULL;
  }
#ifdef DSLIBRIS_DEBUG
  if (t_after_content)
    *t_after_content = osGetTime();
  if (reporter) {
    const text_layout_utils::PerfStats after =
        text_layout_utils::GetPerfStats();
    const epub_parse_perf::Snapshot parse_after = epub_parse_perf::Get();
    const u64 spine_ms = osGetTime() - t_spine_begin;
    const u64 shape_ms = after.shape_ms - spine_layout_before.shape_ms;
    const u64 measure_ms = after.measure_ms - spine_layout_before.measure_ms;
    const u64 break_ms = after.line_break_ms - spine_layout_before.line_break_ms;
    const u64 pre_break_ms =
        after.pre_line_break_ms - spine_layout_before.pre_line_break_ms;
    const u64 layout_ms = shape_ms + measure_ms + break_ms + pre_break_ms;
    const u64 elem_ms =
        parse_after.element_ms - spine_parse_before.element_ms;
    const u64 chardata_ms =
        parse_after.chardata_ms - spine_parse_before.chardata_ms;
    const u64 flush_ms =
        parse_after.flush_ms - spine_parse_before.flush_ms;
    const u32 elem_calls =
        parse_after.element_calls - spine_parse_before.element_calls;
    const u32 chardata_calls =
        parse_after.chardata_calls - spine_parse_before.chardata_calls;
    const u32 flush_calls =
        parse_after.flush_calls - spine_parse_before.flush_calls;
    const u32 overflows =
        parse_after.page_overflows - spine_parse_before.page_overflows;
    const unsigned shape_pct =
        spine_ms ? (unsigned)((shape_ms * 100ULL) / spine_ms) : 0;
    const unsigned measure_pct =
        spine_ms ? (unsigned)((measure_ms * 100ULL) / spine_ms) : 0;
    const unsigned break_pct =
        spine_ms ? (unsigned)(((break_ms + pre_break_ms) * 100ULL) / spine_ms)
                 : 0;
    const unsigned layout_pct =
        spine_ms ? (unsigned)((layout_ms * 100ULL) / spine_ms) : 0;
    const unsigned elem_pct =
        spine_ms ? (unsigned)((elem_ms * 100ULL) / spine_ms) : 0;
    const unsigned chardata_pct =
        spine_ms ? (unsigned)((chardata_ms * 100ULL) / spine_ms) : 0;
    const unsigned flush_pct =
        spine_ms ? (unsigned)((flush_ms * 100ULL) / spine_ms) : 0;
    DBG_LOGF(reporter,
             "EPUB: spine breakdown total=%llums layout=%llums(%u%%) "
             "shape=%llums(%u%%) measure=%llums(%u%%) linebreak=%llums(%u%%) "
             "elem=%llums(%u%%)/%u chardata=%llums(%u%%)/%u "
             "flush=%llums(%u%%)/%u overflows=%u glyphs=%u",
             (unsigned long long)spine_ms, (unsigned long long)layout_ms,
             layout_pct, (unsigned long long)shape_ms, shape_pct,
             (unsigned long long)measure_ms, measure_pct,
             (unsigned long long)(break_ms + pre_break_ms), break_pct,
             (unsigned long long)elem_ms, elem_pct, (unsigned)elem_calls,
             (unsigned long long)chardata_ms, chardata_pct,
             (unsigned)chardata_calls, (unsigned long long)flush_ms,
             flush_pct, (unsigned)flush_calls, (unsigned)overflows,
             (unsigned)(after.shaped_glyphs -
                        spine_layout_before.shaped_glyphs));
  }
#endif
  return rc;
}

int epub(Book *book, std::string name, bool metadataonly) {
  const EpubDeps deps = BuildEpubDeps(book);
  IStatusReporter *reporter = deps.reporter;
  int rc = 0;
  epub_data_t parsedata{};
#ifdef DSLIBRIS_DEBUG
  u64 t_parse_begin = osGetTime();
  u64 t_after_container = t_parse_begin;
  u64 t_after_rootfile = t_parse_begin;
  u64 t_after_content = t_parse_begin;
#endif
  if (reporter && (!metadataonly || EPUB_METADATA_TRACE))
    DBG_LOG(reporter, "EPUB: parse begin");

  bool cache_loaded = false;
  if (!metadataonly && !ShouldAbortEpubOpen(book)) {
    // The metadata cache supplies fields not stored in .epc (author, language,
    // etc.). A complete page/TOC hit needs no ZIP or package XML at all.
    const bool have_metadata = book->TryLoadMetadataFromCache();
    cache_loaded = epub_page_cache::TryLoad(book, name.c_str(),
                                deps.ts ? (int)deps.ts->GetPixelSize() : 0,
                                deps.ts ? (int)deps.ts->linespacing : 0,
                                deps.paragraph_spacing, deps.paragraph_indent,
                                deps.orientation,
                                deps.ts ? (int)deps.ts->margin.left : 0,
                                deps.ts ? (int)deps.ts->margin.right : 0,
                                deps.ts ? (int)deps.ts->margin.top : 0,
                                deps.ts ? (int)deps.ts->margin.bottom : 0,
                                deps.ts ? deps.ts->GetFontFile(TEXT_STYLE_REGULAR).c_str() : NULL);
    if (cache_loaded && have_metadata && !book->GetChapters().empty()) {
      if (reporter)
        DBG_LOGF(reporter, "EPUB: page cache hit pages=%u chapters=%u (no ZIP)",
                 (unsigned)book->GetPageCount(),
                 (unsigned)book->GetChapters().size());
      return FinalizeEpubParse(NULL, &parsedata, book, name, deps, 0, false);
    }
    if (ShouldAbortEpubOpen(book))
      return FinalizeEpubParse(NULL, &parsedata, book, name, deps,
                               BOOK_ERR_CANCELLED, false);
  }

  unzFile uf = NULL;
  rc = OpenEpubArchive(name, &uf);
  if (rc != 0)
    return rc;

  std::string folder;
  rc = LoadEpubPackageForParse(
      uf, book, &parsedata, &folder, metadataonly, deps
#ifdef DSLIBRIS_DEBUG
      ,
      &t_after_container, &t_after_rootfile
#endif
  );
  if (rc == 2) {
    unzClose(uf);
    epub_data_delete(&parsedata);
    return rc;
  }
  if (rc == 0 && ShouldAbortEpubOpen(book))
    return FinalizeEpubParse(uf, &parsedata, book, name, deps,
                             BOOK_ERR_CANCELLED, false);

  if (rc == 0)
    ApplyEpubMetadataOnlyResult(book, parsedata, folder);

  if (metadataonly) {
    unzClose(uf);
    epub_data_delete(&parsedata);
    if (reporter && EPUB_METADATA_TRACE) {
      DBG_LOGF(reporter,
               "EPUB: metadata timing container=%llums opf=%llums total=%llums",
               (unsigned long long)SafeElapsedMs(t_after_container,
                                                 t_parse_begin),
               (unsigned long long)SafeElapsedMs(t_after_rootfile,
                                                 t_after_container),
               (unsigned long long)SafeElapsedMs(osGetTime(), t_parse_begin));
      DBG_LOG(reporter, "EPUB: metadata done");
    }
    return rc;
  }

  if (cache_loaded) {
    if (reporter) {
      DBG_LOGF(reporter, "EPUB: page cache hit pages=%u chapters=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)book->GetChapters().size());
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(reporter, "EPUB: timing cache_total=%llums",
               (unsigned long long)(osGetTime() - t_parse_begin));
#endif
    }
    bool repaired_toc = false;
    if (book->GetChapters().empty() && book->GetPageCount() > 0) {
      std::map<std::string, u16> page_start_by_href;
      BuildPageStartMapFromPackage(parsedata, folder, book, &page_start_by_href);
      ResolveEpubTocFromPackageData(uf, book, parsedata, folder,
                                    page_start_by_href, reporter);
      repaired_toc = !book->GetChapters().empty();
    }
    return FinalizeEpubParse(uf, &parsedata, book, name, deps, 0, repaired_toc);
  }

  std::vector<std::string> hrefs = BuildEpubSpineDocumentList(parsedata);
  if (ShouldAbortEpubOpen(book))
    return FinalizeEpubParse(uf, &parsedata, book, name, deps,
                             BOOK_ERR_CANCELLED, false);
  std::map<std::string, u16> page_start_by_href;

  epub_page_cache::StreamWriter stream_writer;
  // Avoid SD cache writes on the critical first-open path. The pages remain
  // resident for reading, and FinalizeEpubParse marks the cache save pending so
  // it can be saved between reader inputs, or completed when the book is closed.
  const bool use_stream = false;

  rc = ParseEpubSpineDocuments(uf, book, &parsedata, name, folder, hrefs, deps,
                               &page_start_by_href,
                               use_stream ? &stream_writer : NULL, NULL
#ifdef DSLIBRIS_DEBUG
                               ,
                               &t_after_content
#endif
  );
  if (rc == 0 && (ShouldAbortEpubOpen(book) || book->GetPageCount() == 0)) {
    rc = ShouldAbortEpubOpen(book) ? BOOK_ERR_CANCELLED : 1;
    if (reporter) {
      DBG_LOGF(reporter,
               "EPUB: empty-result rc=%d hrefs=%u docs=%u pages=%u aborted=%u",
               rc, (unsigned)hrefs.size(), (unsigned)page_start_by_href.size(),
               (unsigned)book->GetPageCount(),
               ShouldAbortEpubOpen(book) ? 1u : 0u);
    }
  }
  if (rc != 0) {
    stream_writer.Abort();
    return FinalizeEpubParse(uf, &parsedata, book, name, deps, rc, false);
  }
  if (open_cancel_poll::Poll(book, reporter, "epub-content-done")) {
    stream_writer.Abort();
    return FinalizeEpubParse(uf, &parsedata, book, name, deps,
                             BOOK_ERR_CANCELLED, false);
  }
  if (reporter) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: content done pages=%u",
             (unsigned)book->GetPageCount());
    DBG_LOG(reporter, msg);
    DBG_LOGF(reporter,
             "EPUB: timing container=%llums opf=%llums spine=%llums total=%llums docs=%u pages=%u",
             (unsigned long long)SafeElapsedMs(t_after_container,
                                               t_parse_begin),
             (unsigned long long)SafeElapsedMs(t_after_rootfile,
                                               t_after_container),
             (unsigned long long)SafeElapsedMs(t_after_content,
                                               t_after_rootfile),
             (unsigned long long)SafeElapsedMs(t_after_content,
                                               t_parse_begin),
             (unsigned)page_start_by_href.size(),
             (unsigned)book->GetPageCount());
    }


  ResolveEpubTocFromPackageData(uf, book, parsedata, folder, page_start_by_href,
                                reporter);
  if (open_cancel_poll::Poll(book, reporter, "epub-toc-finalize")) {
    stream_writer.Abort();
    return FinalizeEpubParse(uf, &parsedata, book, name, deps,
                             BOOK_ERR_CANCELLED, false);
  }

  bool stream_saved = false;
  if (use_stream) {
    stream_saved = stream_writer.Finalize(book);
    if (!stream_saved)
      stream_writer.Abort();
  }
  return FinalizeEpubParse(uf, &parsedata, book, name, deps, rc,
                           !stream_saved);
}

int epub_extract_cover(Book *book, const std::string &epubpath) {
  return epub_cover::Extract(book, epubpath);
}

namespace epub_parser {
struct Preparation::State {
  Book *book;
  std::string source, path, folder;
  EpubDeps deps;
  epub_data_t data;
  unzFile zip;
  SpineCursor spine;
  std::vector<std::string> hrefs;
  std::map<std::string, u16> page_starts;
  epub_page_cache::LoadSession loader;
  epub_block_cache::Writer writer;
  std::unique_ptr<epub_block_cache::Reader> validator;
  enum Phase {
    Cache,
    Package,
    Spine,
    WriteDocument,
    Toc,
    Commit,
    Validate,
    Finished
  } phase;
  Result result;
  bool cached, metadata, legacy, persistence_failed, writer_open, migration;
  unsigned before;
  std::vector<unsigned> ranges;
  size_t range, residency_budget;
  bool resident;
  u64 started, parse_ms, cache_ms;
  State()
      : book(NULL), data(), zip(NULL), phase(Cache), result(Pending),
        cached(false), metadata(false), legacy(false),
        persistence_failed(false), writer_open(false), migration(false),
        before(0), range(0), residency_budget(0), resident(false),
        started(osGetTime()), parse_ms(0), cache_ms(0) {}
  ~State() {
    if (book)
      book->SetInlineImageProbeZip(NULL);
    if (zip)
      unzClose(zip);
    epub_data_delete(&data);
  }
  bool StartWriter() {
    writer_open = writer.Begin(book, path);
    if (!writer_open) {
      persistence_failed = true;
      writer.Abort();
    }
    return writer_open;
  }
  void Migration() {
    migration = true;
    ranges.push_back(0);
    for (const auto &v : book->GetChapterDocStartPages())
      if (v.second < book->GetPageCount())
        ranges.push_back(v.second);
    ranges.push_back(book->GetPageCount());
    std::sort(ranges.begin(), ranges.end());
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    if (StartWriter() && ranges.size() > 1) {
      writer.AppendDocument(ranges[0], ranges[1]);
      phase = WriteDocument;
    } else {
      phase = Finished;
      result = Complete;
    }
  }
};
Preparation::Preparation() : state_(NULL) {}
Preparation::~Preparation() {
  if (state_ && state_->result == Pending)
    Cancel();
  delete state_;
}
bool Preparation::Begin(Book *book, const std::string &source,
                        size_t residency_budget) {
  if (state_ && state_->result == Pending)
    Cancel();
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_)
    return false;
  State &s = *state_;
  s.book = book;
  s.source = source;
  s.residency_budget = residency_budget;
  if (!book || book->GetPageCount()) {
    s.result = Failed;
    return false;
  }
  book->PrepareForOpen();
  book->CaptureEbookImageSetting();
  s.deps = BuildEpubDeps(book);
  const auto &l = s.deps.layout;
  const auto params = page_cache_utils::BuildLayoutParams(
      source.c_str(), l.pixel_size, l.linespacing, s.deps.paragraph_spacing,
      s.deps.paragraph_indent, s.deps.orientation, l.margin_left,
      l.margin_right, l.margin_top, l.margin_bottom,
      l.regular_font_path.c_str());
  s.path = epub_page_cache::CachePath(book, source.c_str(), params);
  mkdir(paths::GetCacheBaseDir().c_str(), 0777);
  const size_t slash = s.path.find_last_of('/');
  if (slash != std::string::npos)
    mkdir(s.path.substr(0, slash).c_str(), 0777);
  s.metadata = book->TryLoadMetadataFromCache();
  FILE *fp = fopen(s.path.c_str(), "rb");
  bool v23 = false;
  if (fp) {
    u32 magic = 0;
    u16 version = 0;
    fread(&magic, 1, 4, fp);
    fread(&version, 1, 2, fp);
    s.legacy = version == 22;
    v23 = magic == 0x45504347U && version == 23;
    fclose(fp);
  }
  if (v23 && residency_budget) {
    if (!book->StartEpubPageCache(s.path, residency_budget)) {
      s.result = Failed;
      return false;
    }
    s.resident = true;
  } else
    s.loader.Begin(book, s.path);
  return true;
}
Preparation::Result Preparation::GetResult() const {
  return state_ ? state_->result : Failed;
}
bool Preparation::PersistenceFailed() const {
  return state_ && state_->persistence_failed;
}
const char *Preparation::Label() const {
  if (!state_)
    return "cannot prepare book";
  if (state_->result == NoMemory)
    return "not enough memory to load cache";
  if (state_->result == IoError)
    return "cannot read page cache";
  if (state_->result == Complete && state_->persistence_failed)
    return "cache unavailable; A: read / B: back";
  switch (state_->phase) {
  case State::Cache:
    return "loading cache...";
  case State::Validate:
    return "checking cache... B: cancel";
  case State::WriteDocument:
  case State::Commit:
    return "saving cache... B: cancel";
  case State::Toc:
    return "preparing index... B: cancel";
  default:
    return "opening book... B: cancel";
  }
}
unsigned Preparation::Progress() const {
  if (!state_)
    return 0;
  if (state_->phase == State::Cache)
    return state_->resident ? 0 : state_->loader.Progress();
  return state_->migration ? state_->writer.Progress()
                           : (unsigned)state_->spine.next;
}
unsigned Preparation::Total() const {
  if (!state_)
    return 0;
  if (state_->phase == State::Cache)
    return state_->resident ? 0 : state_->loader.Total();
  return state_->migration ? state_->book->GetPageCount()
                           : (unsigned)state_->hrefs.size();
}
void Preparation::Cancel() {
  if (!state_)
    return;
  State &s = *state_;
  s.writer.Abort();
  s.loader.Cancel();
  s.book->SetInlineImageProbeZip(NULL);
  s.book->CancelPendingEpubPageCacheSave();
  s.book->Close();
  s.result = Cancelled;
}
Preparation::Result Preparation::Step() {
  if (!state_ || state_->result != Pending)
    return GetResult();
  State &s = *state_;
  if (ShouldAbortEpubOpen(s.book)) {
    Cancel();
    return Cancelled;
  }
  if (s.phase == State::Cache) {
    auto r = epub_page_cache::LoadSession::Missing;
    if (s.resident) {
      const int result = s.book->AdvanceEpubPageCache();
      if (result > 0)
        return Pending;
      const auto cache_result = s.book->GetEpubPageCacheResult();
      r = result == 0 ? epub_page_cache::LoadSession::Complete
          : cache_result == epub_block_cache::Corrupt
              ? epub_page_cache::LoadSession::Corrupt
          : cache_result == epub_block_cache::NoMemory
              ? epub_page_cache::LoadSession::NoMemory
              : epub_page_cache::LoadSession::IoError;
      if (result < 0) {
        const unsigned session = s.book->GetOpenSessionId();
        s.book->Close();
        s.book->SetOpenSessionId(session);
        s.resident = false;
      }
    } else
      r = s.loader.Step();
    if (r == epub_page_cache::LoadSession::Pending)
      return Pending;
    if (r == epub_page_cache::LoadSession::Cancelled) {
      Cancel();
      return Cancelled;
    }
    if (r == epub_page_cache::LoadSession::NoMemory)
      return s.result = NoMemory;
    if (r == epub_page_cache::LoadSession::IoError)
      return s.result = IoError;
    if (r == epub_page_cache::LoadSession::Corrupt)
      remove(s.path.c_str());
    s.cached = r == epub_page_cache::LoadSession::Complete;
    if (s.cached && s.metadata && !s.book->GetChapters().empty()) {
      if (s.legacy)
        s.Migration();
      else {
        s.phase = State::Finished;
        s.result = Complete;
      }
    } else
      s.phase = State::Package;
  } else if (s.phase == State::Package) {
    if (OpenEpubArchive(s.source, &s.zip))
      return s.result = Failed;
#ifdef DSLIBRIS_DEBUG
    u64 container = 0, root = 0;
#endif
    const int rc =
        LoadEpubPackageForParse(s.zip, s.book, &s.data, &s.folder, false, s.deps
#ifdef DSLIBRIS_DEBUG
                                ,
                                &container, &root
#endif
        );
    if (rc)
      return s.result = Failed;
    ApplyEpubMetadataOnlyResult(s.book, s.data, s.folder);
    if (s.cached) {
      if (s.book->GetChapters().empty()) {
        BuildPageStartMapFromPackage(s.data, s.folder, s.book, &s.page_starts);
        ResolveEpubTocFromPackageData(s.zip, s.book, s.data, s.folder,
                                      s.page_starts, s.deps.reporter);
      }
      if (s.legacy)
        s.Migration();
      else {
        s.phase = State::Finished;
        s.result = Complete;
      }
    } else {
      s.hrefs = BuildEpubSpineDocumentList(s.data);
      s.StartWriter();
      s.phase = State::Spine;
    }
  } else if (s.phase == State::Spine) {
    s.before = s.book->GetPageCount();
    const u64 begin = osGetTime();
    const int rc =
        ParseEpubSpineDocuments(s.zip, s.book, &s.data, s.source, s.folder,
                                s.hrefs, s.deps, &s.page_starts, NULL, &s.spine
#ifdef DSLIBRIS_DEBUG
                                ,
                                NULL
#endif
        );
    s.parse_ms += osGetTime() - begin;
    if (rc != 0 && rc != kSpinePending)
      return s.result = rc == XML_ERROR_NO_MEMORY ? NoMemory : Failed;
    if (s.writer_open) {
      if (s.writer.AppendDocument(s.before, s.book->GetPageCount()))
        s.phase = State::WriteDocument;
      else {
        s.writer.Abort();
        s.writer_open = false;
        s.persistence_failed = true;
      }
    }
    if (!s.writer_open)
      s.phase = s.spine.done ? State::Toc : State::Spine;
  } else if (s.phase == State::WriteDocument) {
    const u64 begin = osGetTime();
    const auto r = s.writer.Step();
    s.cache_ms += osGetTime() - begin;
    if (r == epub_block_cache::Pending)
      return Pending;
    if (r != epub_block_cache::Ready) {
      s.writer.Abort();
      s.writer_open = false;
      s.persistence_failed = true;
      if (s.migration) {
        s.phase = State::Finished;
        s.result = Complete;
      } else
        s.phase = s.spine.done ? State::Toc : State::Spine;
    } else if (s.migration) {
      if (++s.range + 1 < s.ranges.size())
        s.writer.AppendDocument(s.ranges[s.range], s.ranges[s.range + 1]);
      else {
        s.writer.Finish();
        s.phase = State::Commit;
      }
    } else
      s.phase = s.spine.done ? State::Toc : State::Spine;
  } else if (s.phase == State::Toc) {
    if (!s.book->GetPageCount())
      return s.result = Failed;
    ResolveEpubTocFromPackageData(s.zip, s.book, s.data, s.folder,
                                  s.page_starts, s.deps.reporter);
    if (s.writer_open && s.writer.Finish())
      s.phase = State::Commit;
    else {
      s.persistence_failed = true;
      s.phase = State::Finished;
      s.result = Complete;
    }
  } else if (s.phase == State::Commit) {
    const u64 begin = osGetTime();
    const auto r = s.writer.Step();
    s.cache_ms += osGetTime() - begin;
    if (r == epub_block_cache::Pending)
      return Pending;
    if (r != epub_block_cache::Complete) {
      s.persistence_failed = true;
      s.writer.Abort();
    }
    // Release codec and serialized index before validating the published file.
    s.writer.Abort();
    s.writer_open = false;
    s.phase = State::Finished;
    s.result = Complete;

#ifdef DSLIBRIS_DEBUG
    const unsigned prepared_pages = s.book->GetPageCount();
#endif
    if (s.residency_budget && !s.persistence_failed) {
      s.validator.reset(new (std::nothrow) epub_block_cache::Reader);
      if (s.validator &&
          s.validator->Begin(s.book, s.path, s.residency_budget, true) ==
              epub_block_cache::Pending) {
        s.phase = State::Validate;
        s.result = Pending;
      } else
        s.persistence_failed = true;
    }
    DBG_LOGF(s.deps.reporter,
             "EPUB preparation done pages=%u parse_ms=%llu cache_ms=%llu "
             "total_ms=%llu saved=%u",
             prepared_pages, (unsigned long long)s.parse_ms,
             (unsigned long long)s.cache_ms,
             (unsigned long long)(osGetTime() - s.started),
             s.persistence_failed ? 0 : 1);
  } else if (s.phase == State::Validate) {
    const auto result = s.validator->Step();
    if (result == epub_block_cache::Pending)
      return Pending;
    if (result == epub_block_cache::Complete &&
        s.validator->ActivatePreparedPages() &&
        s.book->AdoptPreparedEpubPageCache(s.validator.get())) {
      s.validator.release();
      s.resident = true;
      s.metadata = true;
      s.legacy = false;
      s.phase = State::Cache;
    } else {
      s.validator.reset();
      s.persistence_failed = true;
      s.phase = State::Finished;
      s.result = Complete;
    }
  }

  return s.result;
}
} // namespace epub_parser
