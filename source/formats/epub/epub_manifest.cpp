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

#include "formats/epub/epub_manifest.h"

#include "book/book_xml.h"
#include "book/page.h"
#include "debug_log.h"
#include "formats/common/xml_parse_utils.h"
#include "formats/epub/epub_cover.h"
#include "formats/epub/epub_package_toc_utils.h"
#include "formats/epub/epub_zip_utils.h"
#include "parse.h"
#include "path_utils.h"
#include "shared/parser_limits.h"
#include "shared/status_reporter.h"
#include "shared/text_layout_utils.h"
#include "shared/string_utils.h"
#include "minizip/unzip.h"
#include <3ds.h>
#include <algorithm>
#include <deque>
#include <limits.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

typedef BookParseDeps EpubDeps;

using epub_package_toc_utils::BuildDocPath;
using epub_package_toc_utils::LocateZipEntrySafe;

void epub_data_init(epub_data_t *d) {
  // Reset any leftover heap objects from previous parses.
  epub_data_delete(d);

  d->type = PARSE_CONTAINER;
  d->ctx.push_back(new std::string("TOP"));
  d->docpath = "";
  d->rootfile = "";
  d->title = "";
  d->creator = "";
  d->coverid = "";
  d->tocid = "";
  d->navid = "";
  d->parsed_doc_title = "";
  d->metadataonly = false;
  d->book = nullptr;
}

void epub_data_delete(epub_data_t *d) {
  for (auto *item : d->manifest)
    delete item;
  d->manifest.clear();

  for (auto *itemref : d->spine)
    delete itemref;
  d->spine.clear();

  for (auto *ctx : d->ctx)
    delete ctx;
  d->ctx.clear();
}

void epub_container_start(void *data, const char *el, const char **attr) {
  epub_data_t *d = (epub_data_t *)data;
  if (!strcmp(el, "rootfile"))
    for (int i = 0; attr[i]; i += 2)
      if (!strcmp(attr[i], "full-path"))
        d->rootfile = attr[i + 1];
}

void epub_rootfile_start(void *data, const char *el, const char **attr) {
  epub_data_t *d = (epub_data_t *)data;
  std::string elem = el;
  if (d->ctx.empty())
    return;
  std::string *ctx = d->ctx.back();
  if (!ctx)
    return;

  if ((*ctx == "manifest" || *ctx == "opf:manifest") &&
      (elem == "item" || elem == "opf:item")) {
    epub_item *item = new epub_item;
    d->manifest.push_back(item);
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "id"))
        item->id = attr[i + 1];
      if (!strcmp(attr[i], "href"))
        item->href = attr[i + 1];
      if (!strcmp(attr[i], "media-type"))
        item->media_type = attr[i + 1];
      if (!strcmp(attr[i], "properties"))
        item->properties = attr[i + 1];
    }
    if (!item->properties.empty() && ContainsToken(item->properties, "nav")) {
      d->navid = item->id;
    }
    if (!item->properties.empty() &&
        ContainsToken(item->properties, "cover-image")) {
      d->coverid = item->id;
    }
    if (d->tocid.empty() && item->media_type == "application/x-dtbncx+xml") {
      d->tocid = item->id;
    }
  }

  else if (elem == "spine" || elem == "opf:spine") {
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "toc"))
        d->tocid = attr[i + 1];
    }
  }

  else if ((*ctx == "spine" || *ctx == "opf:spine") &&
           (elem == "itemref" || elem == "opf:itemref")) {
    epub_itemref *itemref = new epub_itemref;
    d->spine.push_back(itemref);
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "idref"))
        itemref->idref = attr[i + 1];
    }
  }

  else if (elem == "dc:title") {
    d->title.clear();
  }

  else if (elem == "dc:creator") {
    d->creator.clear();
  }

  // Capture <meta name="cover" content="cover-image-id"/>
  else if (elem == "meta" || elem == "opf:meta") {
    std::string name_val, content_val;
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "name"))
        name_val = attr[i + 1];
      if (!strcmp(attr[i], "content"))
        content_val = attr[i + 1];
    }
    if (name_val == "cover" && content_val.length()) {
      d->coverid = content_val;
    }
  }
  d->ctx.push_back(new std::string(elem));
}

void epub_rootfile_end(void *data, const char *el) {
  epub_data_t *d = (epub_data_t *)data;
  if (d->ctx.empty())
    return;
  delete d->ctx.back();
  d->ctx.pop_back();
}

void epub_rootfile_char(void *data, const XML_Char *txt, int len) {
  epub_data_t *d = (epub_data_t *)data;
  if (d->ctx.empty())
    return;
  std::string *ctx = d->ctx.back();
  if (!ctx)
    return;

  if (*ctx == "dc:title") {
    d->title.append((char *)txt, len);
  } else if (*ctx == "dc:creator") {
    d->creator.append((char *)txt, len);
  }
}

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

int epub_parse_currentfile(unzFile uf, epub_data_t *epd, const EpubDeps &deps) {
  int rc = 0;
  parsedata_t pd;
  bool log_content_layout = false;
  u64 t_content_begin = 0;
  u16 pages_before = 0;
  u32 chardata_calls_before = 0;
  u64 chardata_ms_before = 0;
  u32 overflow_before = 0;
  text_layout_utils::PerfStats layout_before;
  xml_parse_utils::XmlParserOptions options;
  if (epd->type == PARSE_CONTAINER) {
    options.user_data = epd;
    options.start_element = epub_container_start;
  } else if (epd->type == PARSE_ROOTFILE) {
    options.user_data = epd;
    options.start_element = epub_rootfile_start;
    options.end_element = epub_rootfile_end;
    options.character_data = epub_rootfile_char;
  } else if (epd->type == PARSE_CONTENT) {
    epd->parsed_doc_title.clear();
    InitParsedataWithEpubDeps(&pd, epd->book, deps);
    pd.docpath = epd->docpath;
    log_content_layout = deps.reporter && epd->book;
    if (log_content_layout) {
      t_content_begin = osGetTime();
      pages_before = epd->book->GetPageCount();
      chardata_calls_before = pd.perf_chardata_calls;
      chardata_ms_before = pd.perf_chardata_ms;
      overflow_before = pd.perf_page_overflows;
      layout_before = text_layout_utils::GetPerfStats();
    }
    options.user_data = &pd;
    options.start_element = xml::book::start;
    options.end_element = xml::book::end;
    options.character_data = xml::book::chardata;
    options.default_handler = xml::book::fallback;
    options.processing_instruction = xml::book::instruction;
  } else
    return 0;

  xml_parse_utils::XmlParseResult parse_result =
      xml_parse_utils::ParseXmlZipEntry(uf, options,
                                        parser_limits::kXmlStreamBufferSize);
#ifdef DSLIBRIS_DEBUG
  if (log_content_layout) {
    const text_layout_utils::PerfStats layout_after =
        text_layout_utils::GetPerfStats();
    DBG_LOGF(deps.reporter,
             "EPUB layout: stream=%llums chardata=%llums/%u "
             "shape=%llums/%u break=%llums/%u pre=%llums/%u "
             "measure=%llums/%u glyphs=%u pages=%u overflow_pages=%u path=%s",
             (unsigned long long)(osGetTime() - t_content_begin),
             (unsigned long long)(pd.perf_chardata_ms - chardata_ms_before),
             (unsigned)(pd.perf_chardata_calls - chardata_calls_before),
             (unsigned long long)(layout_after.shape_ms - layout_before.shape_ms),
             (unsigned)(layout_after.shape_calls - layout_before.shape_calls),
             (unsigned long long)(layout_after.line_break_ms -
                                  layout_before.line_break_ms),
             (unsigned)(layout_after.line_break_calls -
                        layout_before.line_break_calls),
             (unsigned long long)(layout_after.pre_line_break_ms -
                                  layout_before.pre_line_break_ms),
             (unsigned)(layout_after.pre_line_break_calls -
                        layout_before.pre_line_break_calls),
             (unsigned long long)(layout_after.measure_ms -
                                  layout_before.measure_ms),
             (unsigned)(layout_after.measure_calls -
                        layout_before.measure_calls),
             (unsigned)(layout_after.shaped_glyphs - layout_before.shaped_glyphs),
             (unsigned)(epd->book->GetPageCount() - pages_before),
             (unsigned)(pd.perf_page_overflows - overflow_before),
             epd->docpath.c_str());
  }
#endif
  if (!parse_result.ok)
    rc = (int)parse_result.error_code;
  if (epd->type == PARSE_CONTENT) {
    epd->parsed_doc_title = Trim(pd.doc_heading);
    if (epd->parsed_doc_title.empty())
      epd->parsed_doc_title = Trim(pd.doc_title);
  }
  return (rc);
}

int LoadEpubPackageData(unzFile uf, Book *book, epub_data_t *parsedata,
                        std::string *opf_folder, const EpubDeps &deps) {
  if (!uf || !parsedata || !opf_folder)
    return 1;

  int rc = unzLocateFile(uf, "META-INF/container.xml", 2); // case-insensitive
  if (rc != UNZ_OK)
    return rc;

  rc = unzOpenCurrentFile(uf);
  if (rc != UNZ_OK)
    return rc;

  epub_data_init(parsedata);
  parsedata->book = book;
  parsedata->type = PARSE_CONTAINER;
  rc = epub_parse_currentfile(uf, parsedata, deps);
  int close_rc = unzCloseCurrentFile(uf);
  if (rc == 0 && close_rc != UNZ_OK)
    rc = close_rc;
  if (rc != 0)
    return rc;
  if (parsedata->rootfile.empty())
    return 1;

  *opf_folder = "";
  size_t pos =
      parsedata->rootfile.find_last_of("/", parsedata->rootfile.length());
  if (pos < parsedata->rootfile.length())
    *opf_folder = parsedata->rootfile.substr(0, pos);

  rc = unzLocateFile(uf, parsedata->rootfile.c_str(), 0);
  if (rc != UNZ_OK)
    return rc;
  rc = unzOpenCurrentFile(uf);
  if (rc != UNZ_OK)
    return rc;

  epub_data_init(parsedata);
  parsedata->book = book;
  parsedata->type = PARSE_ROOTFILE;
  rc = epub_parse_currentfile(uf, parsedata, deps);
  close_rc = unzCloseCurrentFile(uf);
  if (rc == 0 && close_rc != UNZ_OK)
    rc = close_rc;
  return rc;
}

int LoadEpubPackageForParse(
    unzFile uf, Book *book, epub_data_t *parsedata, std::string *folder,
    const EpubDeps &deps
#ifdef DSLIBRIS_DEBUG
    ,
    u64 *t_after_container, u64 *t_after_rootfile
#endif
) {
  if (!uf || !parsedata || !folder)
    return 1;

  int rc = unzLocateFile(uf, "META-INF/container.xml", 2);
  if (rc != UNZ_OK)
    return 2;

  rc = unzOpenCurrentFile(uf);
  epub_data_init(parsedata);
  parsedata->book = book;
  parsedata->type = PARSE_CONTAINER;
  rc = epub_parse_currentfile(uf, parsedata, deps);
  rc = unzCloseCurrentFile(uf);
#ifdef DSLIBRIS_DEBUG
  if (t_after_container)
    *t_after_container = osGetTime();
#endif

  folder->clear();
  size_t pos = parsedata->rootfile.find_last_of("/", parsedata->rootfile.length());
  if (pos < parsedata->rootfile.length())
    *folder = parsedata->rootfile.substr(0, pos);

  rc = unzLocateFile(uf, parsedata->rootfile.c_str(), 0);
  if (rc == UNZ_OK) {
    rc = unzOpenCurrentFile(uf);
    epub_data_init(parsedata);
    parsedata->book = book;
    parsedata->type = PARSE_ROOTFILE;
    epub_parse_currentfile(uf, parsedata, deps);
    rc = unzCloseCurrentFile(uf);
  }
#ifdef DSLIBRIS_DEBUG
  if (t_after_rootfile)
    *t_after_rootfile = osGetTime();
#endif
  return rc;
}

void ApplyEpubMetadataOnlyResult(Book *book, epub_data_t &parsedata,
                                 const std::string &folder) {
  if (!book)
    return;
  if (parsedata.title.length()) {
    book->SetTitle(parsedata.title.c_str());
    if (parsedata.creator.length())
      book->SetAuthor(parsedata.creator);
  }
  std::string coverpath;
  if (epub_cover::FindLikelyImagePath(parsedata, folder, coverpath)) {
    book->coverImagePath = coverpath;
  } else {
    book->coverImagePath.clear();
  }
}

std::vector<std::string>
BuildEpubSpineDocumentList(const epub_data_t &parsedata) {
  std::vector<std::string> hrefs;
  if (!parsedata.spine.empty()) {
    std::unordered_map<std::string, std::string> manifest_href_by_id;
    manifest_href_by_id.reserve(parsedata.manifest.size());
    for (auto *item : parsedata.manifest)
      manifest_href_by_id[item->id] = item->href;

    for (auto *itemref : parsedata.spine) {
      auto it = manifest_href_by_id.find(itemref->idref);
      if (it != manifest_href_by_id.end())
        hrefs.push_back(it->second);
    }
  } else {
    for (auto *item : parsedata.manifest)
      hrefs.push_back(item->href);
  }
  return hrefs;
}
