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
#include "string_utils.h"
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
      parsedata->docpath = path;
      epub_parse_currentfile(uf, parsedata, deps);
      rc = unzCloseCurrentFile(uf);
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

static void ResolveEpubTocFromPackageData(
    unzFile uf, Book *book, epub_data_t &parsedata, const std::string &folder,
    const std::map<std::string, u16> &page_start_by_href,
    IStatusReporter *reporter) {
  if (!epub_limits::kEnableRealTocResolve) {
    if (reporter)
      DBG_LOG(reporter, "EPUB: TOC resolve skipped (safe mode)");
    return;
  }

  std::vector<toc_entry_t> toc_entries;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC resolve begin");

  LoadTocEntriesFromPackage(uf, parsedata, folder, &toc_entries, reporter);
  if (reporter && !toc_entries.empty())
    LogTocEntrySamples(reporter, "TOC package load", toc_entries, 5);
  if (!toc_entries.empty()) {
    std::vector<ChapterEntry> resolved;
    std::map<u16, bool> used_pages;
    size_t empty_title_count = 0;
    for (size_t i = 0; i < toc_entries.size(); i++) {
      std::string key = NormalizePath(toc_entries[i].href);
      if (key.empty())
        continue;
      auto hit = page_start_by_href.find(key);
      if (hit == page_start_by_href.end())
        continue;
      if (used_pages[hit->second])
        continue;
      used_pages[hit->second] = true;

      ChapterEntry entry;
      entry.page = hit->second;
      entry.title = Trim(toc_entries[i].title);
      entry.level = toc_entries[i].level;
      if (entry.title.empty()) {
        empty_title_count++;
        if (reporter && empty_title_count <= 3) {
          LogTocResolveDecision(reporter, i, toc_entries[i], "",
                                "skip-empty-title",
                                true, entry.page, " raw-empty");
        }
        continue;
      }
      resolved.push_back(entry);
    }

    if (!resolved.empty()) {
      book->ClearChapters();
      for (size_t i = 0; i < resolved.size(); i++)
        book->AddChapter(resolved[i].page, resolved[i].title, resolved[i].level);
      if (reporter)
        LogResolvedChapterSamples(reporter, "TOC package chapters", resolved,
                                  5);
    }
  }

  if (reporter) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: toc entries=%u chapters=%u",
             (unsigned)toc_entries.size(),
             (unsigned)book->GetChapters().size());
    DBG_LOG(reporter, msg);
    DBG_LOG(reporter, "EPUB: TOC resolve end");
  }
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

int epub_resolve_toc(Book *book, std::string filepath) {
  if (!book || filepath.empty())
    return 1;
  if (book->format != FORMAT_EPUB)
    return 2;
  if (book->GetChapters().empty())
    return 3;

  const EpubDeps deps = BuildEpubDeps(book);
  IStatusReporter *reporter = deps.reporter;
  IStatusReporter *app = reporter;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC resolve begin");
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC open zip begin");

  unzFile uf = unzOpen(filepath.c_str());
  if (!uf)
    return 4;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC open zip ok");

  epub_data_t parsedata;
  std::string opf_folder;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC package load begin");
  int rc = LoadEpubPackageData(uf, book, &parsedata, &opf_folder, deps);
  if (rc != 0) {
    if (reporter) {
      char msg[96];
      snprintf(msg, sizeof(msg), "EPUB: TOC package load fail rc=%d", rc);
      DBG_LOG(reporter, msg);
    }
    epub_data_delete(&parsedata);
    unzClose(uf);
    return rc;
  }
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC package load ok");

  std::map<std::string, u16> page_start_by_href;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC map build begin");
  BuildPageStartMapFromPackage(parsedata, opf_folder, book,
                               &page_start_by_href);
  if (reporter) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: TOC map build ok size=%u",
             (unsigned)page_start_by_href.size());
    DBG_LOG(reporter, msg);
  }

  std::vector<toc_entry_t> toc_entries;
  if (reporter)
    DBG_LOG(reporter, "EPUB: TOC entries load begin");
  bool loaded =
      LoadTocEntriesFromPackage(uf, parsedata, opf_folder, &toc_entries,
                                reporter);
  if (reporter) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: TOC entries load end loaded=%u count=%u",
             loaded ? 1u : 0u, (unsigned)toc_entries.size());
    DBG_LOG(reporter, msg);
  }
  if (reporter && loaded && !toc_entries.empty())
    LogTocEntrySamples(reporter, "TOC deferred load", toc_entries, 5);
  if (!loaded) {
    if (reporter)
      DBG_LOG(reporter, "EPUB: TOC resolve end (no entries)");
    epub_data_delete(&parsedata);
    unzClose(uf);
    return 5;
  }

  std::vector<ChapterEntry> resolved;
  const std::vector<ChapterEntry> parsed_headings = book->GetChapters();
  std::map<u16, bool> used_pages_non_fragment;
  std::map<std::string, u16> page_start_by_href_lc;
  std::map<std::string, u16> page_start_by_basename_lc;
  std::set<std::string> basename_lc_ambiguous;
  std::vector<u16> doc_starts;
  size_t stat_exact = 0;
  size_t stat_nofrag = 0;
  size_t stat_anchor = 0;
  size_t stat_anchor_miss = 0;
  size_t stat_title_fallback = 0;
  size_t stat_title_global = 0;
  size_t stat_heading_fallback = 0;
  size_t stat_proxy = 0;
  size_t stat_with_fragment = 0;
  size_t stat_lc = 0;
  size_t stat_base = 0;
  size_t stat_skip_unmatched = 0;
  size_t stat_skip_dup = 0;
  size_t stat_title_empty_norm = 0;
  size_t stat_fragment_unresolved = 0;
  std::vector<std::string> unresolved_fragment_samples;
  std::map<std::string, std::map<std::string, std::string>> toc_proxy_cache;
  std::set<std::string> toc_proxy_attempted;

  for (const auto &kv : page_start_by_href) {
    std::string lower_key = ToLowerAscii(kv.first);
    if (page_start_by_href_lc.find(lower_key) == page_start_by_href_lc.end())
      page_start_by_href_lc[lower_key] = kv.second;
    doc_starts.push_back(kv.second);

    std::string base = BasenamePath(lower_key);
    if (base.empty())
      continue;
    if (basename_lc_ambiguous.find(base) != basename_lc_ambiguous.end())
      continue;
    auto hit_base = page_start_by_basename_lc.find(base);
    if (hit_base == page_start_by_basename_lc.end()) {
      page_start_by_basename_lc[base] = kv.second;
    } else if (hit_base->second != kv.second) {
      page_start_by_basename_lc.erase(base);
      basename_lc_ambiguous.insert(base);
    }
  }
  std::sort(doc_starts.begin(), doc_starts.end());
  doc_starts.erase(std::unique(doc_starts.begin(), doc_starts.end()),
                   doc_starts.end());

  const size_t kResolvedMaxEntries = 512;
  const size_t kResolveDecisionDiagLimit = 8;
  const size_t kResolveProblemDiagLimit = 6;
  const u16 total_pages = book->GetPageCount();
  bool have_last_resolved_page = false;
  u16 last_resolved_page = 0;
  size_t resolve_problem_logs = 0;

  for (size_t i = 0; i < toc_entries.size(); i++) {
    std::string raw_title = Trim(toc_entries[i].title);
    std::string resolve_method = "skip-empty-raw";
    if (raw_title.empty()) {
      if (app && resolve_problem_logs < kResolveProblemDiagLimit) {
        LogTocResolveDecision(app, i, toc_entries[i], "", resolve_method.c_str(),
                              false, 0, " raw-empty");
        resolve_problem_logs++;
      }
      continue;
    }
    std::string title_match = NormalizeTocTitle(raw_title);
    if (title_match.empty()) {
      title_match = raw_title;
      stat_title_empty_norm++;
      resolve_method = "norm-fallback-raw";
      if (app && stat_title_empty_norm <= 3) {
        char msg[160];
        std::string clip = ClipForDiag(raw_title, 100);
        snprintf(msg, sizeof(msg), "EPUB: TOC title normalize fallback [%u]=%s",
                 (unsigned)stat_title_empty_norm, clip.c_str());
        DBG_LOG(app, msg);
      }
    }
    std::string title = title_match;

    u16 page = 0;
    bool have_page = false;
    bool page_from_doc_start = false;
    bool anchor_lookup_failed = false;
    std::string mapped_doc_key;

    const bool has_fragment =
        toc_entries[i].href.find('#') != std::string::npos;
    if (has_fragment)
      stat_with_fragment++;
    if (has_fragment) {
      u16 anchor_page = 0;
      if (book->FindChapterAnchorPage(toc_entries[i].href, &anchor_page)) {
        page = anchor_page;
        have_page = true;
        stat_anchor++;
        resolve_method = "anchor";
      } else {
        stat_anchor_miss++;
        anchor_lookup_failed = true;
        resolve_method = "anchor-miss";
      }
    }

    std::string key = NormalizePath(toc_entries[i].href);
    if (!key.empty()) {
      if (!have_page) {
        auto hit = page_start_by_href.find(key);
        if (hit != page_start_by_href.end()) {
          page = hit->second;
          have_page = true;
          stat_exact++;
          resolve_method = "exact";
        }
      }
      if (!have_page) {
        std::string key_no_fragment =
            NormalizePath(StripFragmentAndQuery(toc_entries[i].href));
        if (!key_no_fragment.empty()) {
          auto hit_nofrag = page_start_by_href.find(key_no_fragment);
          if (hit_nofrag != page_start_by_href.end()) {
            page = hit_nofrag->second;
            have_page = true;
            stat_nofrag++;
            page_from_doc_start = true;
            mapped_doc_key = key_no_fragment;
            resolve_method = "nofrag";
          }
        }
      }
      if (!have_page) {
        std::string key_lc = ToLowerAscii(key);
        auto hit_lc = page_start_by_href_lc.find(key_lc);
        if (hit_lc != page_start_by_href_lc.end()) {
          page = hit_lc->second;
          have_page = true;
          stat_lc++;
          resolve_method = "lower";
        }
      }
      if (!have_page) {
        std::string key_no_fragment_lc = ToLowerAscii(
            NormalizePath(StripFragmentAndQuery(toc_entries[i].href)));
        auto hit_nofrag_lc = page_start_by_href_lc.find(key_no_fragment_lc);
        if (hit_nofrag_lc != page_start_by_href_lc.end()) {
          page = hit_nofrag_lc->second;
          have_page = true;
          stat_lc++;
          page_from_doc_start = true;
          mapped_doc_key = key_no_fragment_lc;
          resolve_method = "lower-nofrag";
        }
      }
      if (!have_page) {
        std::string base = BasenamePath(ToLowerAscii(key));
        if (!base.empty()) {
          auto hit_base = page_start_by_basename_lc.find(base);
          if (hit_base != page_start_by_basename_lc.end()) {
            page = hit_base->second;
            have_page = true;
            stat_base++;
            resolve_method = "basename";
          }
        }
      }
    }

    bool mapped_is_toc_doc = PathLooksLikeTocDocForFallback(mapped_doc_key);
    if (has_fragment && anchor_lookup_failed && have_page &&
        page_from_doc_start && mapped_is_toc_doc) {
      std::string fragment = ExtractHrefFragment(toc_entries[i].href);
      std::string proxy_href;
      if (LookupTocProxyHref(uf, mapped_doc_key, fragment, &toc_proxy_cache,
                             &toc_proxy_attempted, &proxy_href, app)) {
        u16 proxy_page = 0;
        bool proxy_ok = false;
        if (book->FindChapterAnchorPage(proxy_href, &proxy_page)) {
          proxy_ok = true;
        } else {
          std::string proxy_key = NormalizePath(proxy_href);
          if (!proxy_key.empty()) {
            auto hit_exact = page_start_by_href.find(proxy_key);
            if (hit_exact != page_start_by_href.end()) {
              proxy_page = hit_exact->second;
              proxy_ok = true;
            }
          }
          if (!proxy_ok) {
            std::string proxy_nofrag =
                NormalizePath(StripFragmentAndQuery(proxy_href));
            if (!proxy_nofrag.empty()) {
              auto hit_nofrag = page_start_by_href.find(proxy_nofrag);
              if (hit_nofrag != page_start_by_href.end()) {
                proxy_page = hit_nofrag->second;
                proxy_ok = true;
              }
            }
          }
        }

        if (proxy_ok) {
          page = proxy_page;
          have_page = true;
          page_from_doc_start = false;
          anchor_lookup_failed = false;
          stat_proxy++;
          resolve_method = "proxy";
        }
      }
    }

    if (has_fragment && anchor_lookup_failed && have_page &&
        page_from_doc_start) {
      u16 heading_page = 0;
      u16 min_heading_page = page;
      if (have_last_resolved_page && last_resolved_page > min_heading_page)
        min_heading_page = last_resolved_page;

      if (FindChapterPageFromParsedHeadings(parsed_headings, title_match,
                                            min_heading_page, &heading_page)) {
        page = heading_page;
        stat_heading_fallback++;
        page_from_doc_start = false;
        anchor_lookup_failed = false;
        resolve_method = "heading-fallback";
      }
    }

    if (has_fragment && anchor_lookup_failed && have_page &&
        page_from_doc_start) {
      u16 title_page = 0;
      bool got_title = false;
      if (!mapped_is_toc_doc) {
        got_title = FindTocTitlePageInDocRange(book, page, doc_starts,
                                               title_match, &title_page);
      } else {
        // Some EPUBs point TOC entries to an index/toc doc with numeric
        // fragments (#3, #7...) that are not real content anchors.
        // In that case, resolve by searching chapter title in the book body.
        u16 from_page = (u16)(page + 1);
        if (have_last_resolved_page && total_pages > 0) {
          u16 next_page = (last_resolved_page + 1 < total_pages)
                              ? (u16)(last_resolved_page + 1)
                              : last_resolved_page;
          if (next_page > from_page)
            from_page = next_page;
        }
        u16 search_end = total_pages;
        if (total_pages > 0 && from_page < total_pages) {
          size_t remaining_entries = toc_entries.size() - i;
          u16 remaining_pages =
              (from_page < total_pages) ? (u16)(total_pages - from_page) : 1;
          u16 expected_span = 1;
          if (remaining_entries > 0)
            expected_span = (u16)std::max<size_t>(1, (size_t)remaining_pages /
                                                         remaining_entries);
          u16 search_window = (u16)(expected_span * 4);
          if (search_window < 24)
            search_window = 24;
          if (search_window > 128)
            search_window = 128;
          unsigned int end_u32 = (unsigned int)from_page + search_window;
          if (end_u32 > (unsigned int)total_pages)
            end_u32 = total_pages;
          search_end = (u16)end_u32;
        }
        got_title = FindTocTitlePageGlobal(book, title_match, from_page,
                                           &title_page, false, search_end);
        if (!got_title && search_end < total_pages) {
          got_title = FindTocTitlePageGlobal(book, title_match, from_page,
                                             &title_page, false, total_pages);
        }
        if (got_title)
          stat_title_global++;
      }
      if (got_title) {
        page = title_page;
        stat_title_fallback++;
        resolve_method = mapped_is_toc_doc ? "title-global" : "title-doc";
      } else if (unresolved_fragment_samples.size() < 3) {
        std::string sample = toc_entries[i].href + " | " + title_match;
        unresolved_fragment_samples.push_back(sample);
      }

      // If this came from a TOC/index document and we still couldn't resolve
      // by title, avoid emitting a broken chapter entry (often page 0).
      if (!got_title && mapped_is_toc_doc) {
        have_page = false;
        resolve_method = "skip-toc-doc-unmatched";
      }
    }

    if (!have_page) {
      stat_skip_unmatched++;
      if (has_fragment)
        stat_fragment_unresolved++;
      if (app && resolve_problem_logs < kResolveProblemDiagLimit) {
        const char *note = anchor_lookup_failed ? " anchor-miss" : "";
        LogTocResolveDecision(app, i, toc_entries[i], title_match,
                              resolve_method.c_str(), false, 0, note);
        resolve_problem_logs++;
      }
      continue;
    }
    if (!has_fragment && used_pages_non_fragment[page]) {
      stat_skip_dup++;
      if (app && resolve_problem_logs < kResolveProblemDiagLimit) {
        LogTocResolveDecision(app, i, toc_entries[i], title_match,
                              "skip-dup-page", true, page, "");
        resolve_problem_logs++;
      }
      continue;
    }
    if (!has_fragment)
      used_pages_non_fragment[page] = true;

    ChapterEntry entry;
    entry.page = page;
    entry.title = title;
    entry.level = toc_entries[i].level;
    resolved.push_back(entry);
    if (app && i < kResolveDecisionDiagLimit) {
      LogTocResolveDecision(app, i, toc_entries[i], title_match,
                            resolve_method.c_str(), true, page,
                            page_from_doc_start ? " doc-start" : "");
    }
    have_last_resolved_page = true;
    last_resolved_page = page;
    if (resolved.size() >= kResolvedMaxEntries)
      break;
  }

  if (resolved.empty()) {
    if (app)
      DBG_LOG(app, "EPUB: TOC resolve end (unmatched)");
    epub_data_delete(&parsedata);
    unzClose(uf);
    return 6;
  }

  book->ClearChapters();
  for (size_t i = 0; i < resolved.size(); i++) {
    book->AddChapter(resolved[i].page, resolved[i].title, resolved[i].level);
  }
  if (app)
    LogResolvedChapterSamples(app, "TOC resolved chapters", resolved, 6);

  if (app) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: toc entries=%u chapters=%u",
             (unsigned)toc_entries.size(),
             (unsigned)book->GetChapters().size());
    DBG_LOG(app, msg);
    char map_msg[192];
    snprintf(map_msg, sizeof(map_msg),
             "EPUB: TOC map stats anchor=%u exact=%u nofrag=%u lower=%u "
             "base=%u proxy=%u headfb=%u titlefb=%u titleg=%u skip=%u dup=%u",
             (unsigned)stat_anchor, (unsigned)stat_exact, (unsigned)stat_nofrag,
             (unsigned)stat_lc, (unsigned)stat_base, (unsigned)stat_proxy,
             (unsigned)stat_heading_fallback, (unsigned)stat_title_fallback,
             (unsigned)stat_title_global, (unsigned)stat_skip_unmatched,
             (unsigned)stat_skip_dup);
    DBG_LOG(app, map_msg);

    size_t direct_count = stat_anchor + stat_exact + stat_nofrag + stat_lc +
                          stat_base + stat_proxy;
    size_t heuristic_count = stat_heading_fallback + stat_title_fallback;
    const char *quality = "strong";
    TocQuality quality_enum = TOC_QUALITY_STRONG;
    if (heuristic_count > 0 || stat_skip_unmatched > 0) {
      quality = "mixed";
      quality_enum = TOC_QUALITY_MIXED;
      if (heuristic_count >= (resolved.size() / 2) ||
          stat_skip_unmatched >= (toc_entries.size() / 3)) {
        quality = "heuristic";
        quality_enum = TOC_QUALITY_HEURISTIC;
      }
    }
    book->SetTocConfidence(quality_enum, (u16)direct_count,
                           (u16)heuristic_count, (u16)stat_skip_unmatched);
    char quality_msg[176];
    snprintf(quality_msg, sizeof(quality_msg),
             "EPUB: TOC quality=%s direct=%u heuristic=%u unresolved=%u",
             quality, (unsigned)direct_count, (unsigned)heuristic_count,
             (unsigned)stat_skip_unmatched);
    DBG_LOG(app, quality_msg);

    if (stat_fragment_unresolved > 0) {
      char warn_msg[160];
      snprintf(warn_msg, sizeof(warn_msg),
               "EPUB: TOC fragments unresolved=%u anchor_map=%u titlefb=%u",
               (unsigned)stat_fragment_unresolved,
               (unsigned)book->GetChapterAnchorCount(),
               (unsigned)stat_title_fallback);
      DBG_LOG(app, warn_msg);
      for (size_t i = 0; i < unresolved_fragment_samples.size(); i++) {
        char sample_msg[192];
        std::string clipped = ClipForDiag(unresolved_fragment_samples[i], 100);
        snprintf(sample_msg, sizeof(sample_msg),
                 "EPUB: TOC unresolved href[%u]=%s", (unsigned)i,
                 clipped.c_str());
        DBG_LOG(app, sample_msg);
      }
    }
    DBG_LOG(app, "EPUB: TOC resolve end");
  }

  epub_data_delete(&parsedata);
  unzClose(uf);
  return 0;
}
