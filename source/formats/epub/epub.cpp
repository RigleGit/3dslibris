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
#include "formats/epub/epub_cache.h"
#include "formats/epub/epub_manifest.h"
#include "formats/epub/epub_toc.h"

#include "base64_utils.h"
#include "book/book.h"
#include "book/book_parse_deps.h"
#include "book/book_xml.h"
#include "debug_log.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/xml_parse_utils.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/epub/epub_cover.h"
#include "formats/epub/epub_limits.h"
#include "formats/epub/epub_ncx_parser.h"
#include "formats/epub/epub_package_toc_utils.h"
#include "formats/epub/epub_toc_diag_utils.h"
#include "formats/epub/epub_toc_package_loader_utils.h"
#include "formats/epub/epub_toc_title_match_utils.h"
#include "formats/epub/epub_zip_utils.h"
#include "path_utils.h"
#include "book/page.h"
#include "shared/parser_limits.h"
#include "shared/status_reporter.h"
#include "shared/text_layout_utils.h"
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

typedef BookParseDeps EpubDeps;

static EpubDeps BuildEpubDeps(Book *book) { return BuildBookParseDeps(book); }
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

static void InitParsedataWithEpubDeps(parsedata_t *parsedata, Book *book,
                                      const EpubDeps &deps) {
  if (!parsedata)
    return;
  parse_init(parsedata);
  parsedata->book = book;
  parsedata->reporter = deps.reporter;
  parsedata->ts = deps.ts;
  parsedata->prefs = deps.prefs;
}

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

static int ParseEpubSpineDocuments(
    unzFile uf, Book *book, epub_data_t *parsedata,
    const std::string &archive_path, const std::string &folder,
    const std::vector<std::string> &hrefs, const EpubDeps &deps,
    std::map<std::string, u16> *page_start_by_href
#ifdef DSLIBRIS_DEBUG
    ,
    u64 *t_after_content
#endif
) {
  if (!uf || !book || !parsedata || !page_start_by_href)
    return 1;

  IStatusReporter *app = deps.reporter;
  epub_zip_utils::ZipEntryIndex zip_index;
  int rc = 0;
  parsedata->ctx.clear();
  parsedata->book = book;
  parsedata->type = PARSE_CONTENT;
  book->ClearChapters();
  book->ClearChapterDocStartPages();
  book->ClearInlineImages();
  page_start_by_href->clear();

  int chapter_num = 1;
  size_t spine_doc_index = 0;
  unzFile inline_probe_uf = unzOpen(archive_path.c_str());
  book->SetInlineImageProbeZip(inline_probe_uf);

  for (size_t i = 0; i < hrefs.size(); i++) {
    spine_doc_index++;
    const std::string &href = hrefs[i];
    size_t pos = href.find_last_of('.');
    if (pos >= href.length())
      continue;

    std::string path = BuildDocPath(folder, href.c_str());
    std::string path_key = NormalizePath(path);
    if (LocateZipEntrySafe(uf, path, app, "SPINE", &zip_index)) {
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
      const int parse_rc = epub_parse_currentfile(uf, parsedata, deps);
      const int close_rc = unzCloseCurrentFile(uf);
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
#ifdef DSLIBRIS_DEBUG
      if (app) {
        u64 doc_ms = osGetTime() - t_doc_begin;
        unsigned int pages_added =
            (unsigned int)(book->GetPageCount() - chapter_start_page);
        unsigned int anchors_added =
            (unsigned int)(book->GetChapterAnchorCount() - anchors_before);
        if (doc_ms >= 200 || pages_added >= 40 || anchors_added >= 100) {
          DBG_LOGF(app,
                   "EPUB: spine doc %u/%u ms=%llums pages=%u anchors=%u path=%s",
                   (unsigned)spine_doc_index, (unsigned)hrefs.size(),
                   (unsigned long long)doc_ms, pages_added, anchors_added,
                   path.c_str());
        }
      }
#endif
    } else if (app) {
      char msg[256];
      sprintf(msg, "NOT FOUND IN ZIP: %s", path.c_str());
      DBG_LOG(app, msg);
    }
  }

  book->SetInlineImageProbeZip(NULL);
  if (inline_probe_uf)
    unzClose(inline_probe_uf);
#ifdef DSLIBRIS_DEBUG
  if (t_after_content)
    *t_after_content = osGetTime();
#endif
  return rc;
}

int epub(Book *book, std::string name, bool metadataonly) {
  const EpubDeps deps = BuildEpubDeps(book);
  IStatusReporter *reporter = deps.reporter;
  int rc = 0;
  static epub_data_t parsedata;
#ifdef DSLIBRIS_DEBUG
  u64 t_parse_begin = osGetTime();
  u64 t_after_container = t_parse_begin;
  u64 t_after_rootfile = t_parse_begin;
  u64 t_after_content = t_parse_begin;
#endif
  if (reporter)
    DBG_LOG(reporter, "EPUB: parse begin");

  unzFile uf = NULL;
  rc = OpenEpubArchive(name, &uf);
  if (rc != 0)
    return rc;

  std::string folder;
  rc = LoadEpubPackageForParse(
      uf, book, &parsedata, &folder, deps
#ifdef DSLIBRIS_DEBUG
      ,
      &t_after_container, &t_after_rootfile
#endif
  );
  if (rc == 2) {
    unzClose(uf);
    return rc;
  }

  if (metadataonly) {
    ApplyEpubMetadataOnlyResult(book, parsedata, folder);
    unzClose(uf);
    epub_data_delete(&parsedata);
    if (reporter) {
      DBG_LOGF(reporter,
               "EPUB: metadata timing container=%llums opf=%llums total=%llums",
               (unsigned long long)(t_after_container - t_parse_begin),
               (unsigned long long)(t_after_rootfile - t_after_container),
               (unsigned long long)(osGetTime() - t_parse_begin));
      DBG_LOG(reporter, "EPUB: metadata done");
    }
    return rc;
  }

  if (epub_page_cache::TryLoad(book, name.c_str(),
                                deps.ts ? (int)deps.ts->GetPixelSize() : 0,
                                deps.ts ? (int)deps.ts->linespacing : 0,
                                deps.paragraph_spacing, deps.paragraph_indent,
                                deps.orientation,
                                deps.ts ? (int)deps.ts->margin.left : 0,
                                deps.ts ? (int)deps.ts->margin.right : 0,
                                deps.ts ? (int)deps.ts->margin.top : 0,
                                deps.ts ? (int)deps.ts->margin.bottom : 0,
                                deps.ts ? deps.ts->GetFontFile(TEXT_STYLE_REGULAR).c_str() : NULL)) {
    if (reporter) {
      DBG_LOGF(reporter, "EPUB: page cache hit pages=%u chapters=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)book->GetChapters().size());
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(reporter, "EPUB: timing cache_total=%llums",
               (unsigned long long)(osGetTime() - t_parse_begin));
#endif
    }
    return FinalizeEpubParse(uf, &parsedata, book, name, deps, 0, false);
  }

  std::vector<std::string> hrefs = BuildEpubSpineDocumentList(parsedata);
  std::map<std::string, u16> page_start_by_href;
  rc = ParseEpubSpineDocuments(
      uf, book, &parsedata, name, folder, hrefs, deps, &page_start_by_href
#ifdef DSLIBRIS_DEBUG
      ,
      &t_after_content
#endif
  );
  if (rc != 0)
    return FinalizeEpubParse(uf, &parsedata, book, name, deps, rc, false);
  if (reporter) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: content done pages=%u",
             (unsigned)book->GetPageCount());
    DBG_LOG(reporter, msg);
    DBG_LOGF(reporter,
             "EPUB: timing container=%llums opf=%llums spine=%llums total=%llums docs=%u pages=%u",
             (unsigned long long)(t_after_container - t_parse_begin),
             (unsigned long long)(t_after_rootfile - t_after_container),
             (unsigned long long)(t_after_content - t_after_rootfile),
             (unsigned long long)(t_after_content - t_parse_begin),
             (unsigned)page_start_by_href.size(),
             (unsigned)book->GetPageCount());
  }

  ResolveEpubTocFromPackageData(uf, book, parsedata, folder, page_start_by_href,
                                reporter);
  return FinalizeEpubParse(uf, &parsedata, book, name, deps, rc, true);
}

int epub_extract_cover(Book *book, const std::string &epubpath) {
  return epub_cover::Extract(book, epubpath);
}
