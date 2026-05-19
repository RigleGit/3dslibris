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
#include "book/epub_css_class_map.h"
#include "book/page.h"
#include "shared/debug_log.h"
#include "formats/common/book_error.h"
#include "formats/common/html_entity_utils.h"
#include "formats/common/xml_parse_utils.h"
#include "shared/open_cancel_poll.h"
#include "formats/epub/epub_cover.h"
#include "formats/epub/epub_limits.h"
#include "formats/epub/epub_package_toc_utils.h"
#include "formats/epub/epub_zip_utils.h"
#include "parse.h"
#include "shared/path_utils.h"
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

#ifndef EPUB_CSS_TRACE
#define EPUB_CSS_TRACE 0
#endif

#ifndef EPUB_LAYOUT_PROFILE
#define EPUB_LAYOUT_PROFILE 0
#endif

#ifdef DSLIBRIS_DEBUG
namespace epub_parse_perf {
// Process-wide aggregator for parsedata_t perf counters across a single
// EPUB open. ParseEpubSpineDocuments snapshots before the spine loop and
// diffs after to attribute the non-layout portion of spine time (XML
// element dispatch, character data emission, page flush).
struct Snapshot {
  u64 chardata_ms;
  u64 element_ms;
  u64 flush_ms;
  u32 chardata_calls;
  u32 element_calls;
  u32 flush_calls;
  u32 page_overflows;
};
static Snapshot g_accum = {0, 0, 0, 0, 0, 0, 0};
Snapshot Get() { return g_accum; }
void Add(const parsedata_t &pd, const Snapshot &before_doc) {
  g_accum.chardata_ms += pd.perf_chardata_ms - before_doc.chardata_ms;
  g_accum.element_ms += pd.perf_element_ms - before_doc.element_ms;
  g_accum.flush_ms += pd.perf_flush_ms - before_doc.flush_ms;
  g_accum.chardata_calls += pd.perf_chardata_calls - before_doc.chardata_calls;
  g_accum.element_calls += pd.perf_element_calls - before_doc.element_calls;
  g_accum.flush_calls += pd.perf_flush_calls - before_doc.flush_calls;
  g_accum.page_overflows +=
      pd.perf_page_overflows - before_doc.page_overflows;
}
} // namespace epub_parse_perf
#endif

typedef BookParseDeps EpubDeps;

using epub_package_toc_utils::BuildDocPath;
using epub_package_toc_utils::LocateZipEntrySafe;
using epub_package_toc_utils::ReadZipEntryText;

namespace {

// Returns hrefs of all CSS stylesheets linked in xhtml_text.
// Skips <link> tags whose type= is present but not "text/css"
// (e.g. Adobe page-template.xpgt uses a vendor MIME type).
std::vector<std::string> ExtractLinkStylesheetHrefs(const std::string &xhtml_text) {
  std::vector<std::string> result;
  const char *s = xhtml_text.c_str();
  size_t len = xhtml_text.size();
  size_t i = 0;
  while (i < len) {
    const char *link_tag = strstr(s + i, "<link");
    if (!link_tag)
      break;
    size_t tag_pos = (size_t)(link_tag - s);
    const char *tag_end = strchr(link_tag, '>');
    if (!tag_end)
      break;
    std::string tag(link_tag, (size_t)(tag_end - link_tag + 1));
    std::string tag_lc = ToLowerAscii(tag);
    if (tag_lc.find("rel=\"stylesheet\"") != std::string::npos ||
        tag_lc.find("rel='stylesheet'") != std::string::npos) {
      bool is_css = true;
      size_t type_pos = tag_lc.find("type=");
      if (type_pos != std::string::npos) {
        type_pos += 5;
        if (type_pos < tag_lc.size()) {
          char tq = tag_lc[type_pos];
          if (tq == '"' || tq == '\'') {
            size_t tv_start = type_pos + 1;
            size_t tv_end = tag_lc.find(tq, tv_start);
            if (tv_end != std::string::npos)
              is_css = (tag_lc.substr(tv_start, tv_end - tv_start) == "text/css");
          }
        }
      }
      if (is_css) {
        size_t href_pos = tag_lc.find("href=");
        if (href_pos != std::string::npos) {
          href_pos += 5;
          char q = tag[href_pos];
          if (q == '"' || q == '\'') {
            size_t val_start = href_pos + 1;
            size_t val_end = tag.find(q, val_start);
            if (val_end != std::string::npos) {
              std::string href = tag.substr(val_start, val_end - val_start);
              if (!href.empty())
                result.push_back(href);
            }
          }
        }
      }
    }
    i = tag_pos + 1;
  }
  return result;
}

// Reads an XHTML zip entry in 4KB chunks and returns all CSS stylesheet hrefs
// found in the <head>. Stops at </head> or after 8KB so only the head section
// is decompressed. Non-CSS stylesheet types (e.g. .xpgt) are filtered out.
std::vector<std::string> ScanXhtmlHeadForCssHrefs(unzFile uf,
                                                   const std::string &xhtml_path,
                                                   IStatusReporter *reporter) {
#if !EPUB_CSS_TRACE
  (void)reporter;
#endif
#if EPUB_CSS_TRACE
  DBG_LOGF(reporter, "EPUB: CSS-SCAN read begin %s", xhtml_path.c_str());
#endif
  epub_zip_utils::ZipEntryIndex zip_index;
  if (!epub_zip_utils::LocateSafe(uf, xhtml_path, &zip_index)) {
#if EPUB_CSS_TRACE
    DBG_LOG(reporter, "EPUB: CSS-SCAN locate fail");
#endif
    return {};
  }
#if EPUB_CSS_TRACE
  DBG_LOG(reporter, "EPUB: CSS-SCAN locate ok");
#endif

  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return {};

  static const size_t kChunkSize = 4096;
  static const size_t kMaxScanBytes = 8192;
  char chunk[kChunkSize];
  std::string buf;
  buf.reserve(kMaxScanBytes);

  while (buf.size() < kMaxScanBytes) {
    int n = unzReadCurrentFile(uf, chunk, (unsigned)kChunkSize);
    if (n <= 0)
      break;
    buf.append(chunk, (size_t)n);
    if (buf.find("</head>") != std::string::npos ||
        buf.find("</HEAD>") != std::string::npos)
      break;
  }

  unzCloseCurrentFile(uf);
#if EPUB_CSS_TRACE
  DBG_LOGF(reporter, "EPUB: CSS-SCAN read ok bytes=%u", (unsigned)buf.size());
#endif
  return ExtractLinkStylesheetHrefs(buf);
}

void LoadCssClassMapForDoc(const std::string &archive_path,
                           const std::string &xhtml_path,
                           IStatusReporter *reporter,
                           epub_data_t *epd,
                           epub_css_class_map::CssClassMap *out,
                           unzFile external_scan_uf) {
  out->clear();
  if (archive_path.empty() || xhtml_path.empty())
    return;

  std::string xhtml_folder;
  size_t slash = xhtml_path.find_last_of('/');
  if (slash != std::string::npos)
    xhtml_folder = xhtml_path.substr(0, slash);

  // Check the per-doc href cache. An empty vector means "already scanned, no CSS".
  if (epd) {
    auto href_it = epd->css_href_by_doc.find(xhtml_path);
    if (href_it != epd->css_href_by_doc.end()) {
      for (const std::string &css_path : href_it->second) {
        auto css_it = epd->css_class_map_by_path.find(css_path);
        if (css_it != epd->css_class_map_by_path.end()) {
          for (const auto &kv : css_it->second)
            (*out)[kv.first] = kv.second;
        }
      }
      return;
    }
  }

  // Use a caller-provided handle if available to avoid opening the zip for
  // every spine document. Fall back to opening our own handle if not provided.
  const bool owns_scan_uf = (external_scan_uf == NULL);
  unzFile scan_uf = owns_scan_uf ? unzOpen(archive_path.c_str()) : external_scan_uf;
  if (!scan_uf)
    return;

  std::vector<std::string> css_hrefs =
      ScanXhtmlHeadForCssHrefs(scan_uf, xhtml_path, reporter);

  // Resolve relative hrefs to full archive paths and deduplicate.
  std::vector<std::string> css_paths;
  for (const std::string &href : css_hrefs) {
    std::string css_path = NormalizePath(xhtml_folder + "/" + href);
    if (!css_path.empty())
      css_paths.push_back(css_path);
  }

  if (epd)
    epd->css_href_by_doc[xhtml_path] = css_paths;

  for (const std::string &css_path : css_paths) {
    // Use cached parse result if available.
    if (epd) {
      auto css_it = epd->css_class_map_by_path.find(css_path);
      if (css_it != epd->css_class_map_by_path.end()) {
        for (const auto &kv : css_it->second)
          (*out)[kv.first] = kv.second;
        continue;
      }
    }

    std::string css_text;
    epub_zip_utils::ZipEntryIndex css_index;
    bool ok =
        ReadZipEntryText(scan_uf, css_path, css_text,
                         EPUB_CSS_TRACE ? reporter : NULL, "CSS-LOAD",
                         &css_index);
    if (!ok || css_text.empty())
      continue;

    epub_css_class_map::CssClassMap parsed;
    epub_css_class_map::ParseCssIntoClassMap(css_text.c_str(), css_text.size(),
                                             &parsed);
    if (epd)
      epd->css_class_map_by_path[css_path] = parsed;
    for (const auto &kv : parsed)
      (*out)[kv.first] = kv.second;
  }

  if (owns_scan_uf) unzClose(scan_uf);
}

void NormalizeHtmlEntityChunkForXml(const std::string &chunk, bool final,
                                    void *transform_ctx, std::string *out) {
  html_entity_utils::ChunkedEntityNormalizerState *state =
      static_cast<html_entity_utils::ChunkedEntityNormalizerState *>(
          transform_ctx);
  html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(chunk, final, state,
                                                           out);
}

} // namespace

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
  d->css_href_by_doc.clear();
  d->css_class_map_by_path.clear();
  d->metadataonly = false;
  d->metadata_parse_complete = false;
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
  d->css_href_by_doc.clear();
  d->css_class_map_by_path.clear();
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
  if (d->metadata_parse_complete)
    return;
  std::string elem = el;
  if (d->ctx.empty())
    return;
  std::string *ctx = d->ctx.back();
  if (!ctx)
    return;

  if (d->metadataonly && (elem == "spine" || elem == "opf:spine")) {
    d->metadata_parse_complete = true;
    return;
  }

  if ((*ctx == "manifest" || *ctx == "opf:manifest") &&
      (elem == "item" || elem == "opf:item")) {
    std::string id;
    std::string href;
    std::string media_type;
    std::string properties;
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "id"))
        id = attr[i + 1];
      if (!strcmp(attr[i], "href"))
        href = attr[i + 1];
      if (!strcmp(attr[i], "media-type"))
        media_type = attr[i + 1];
      if (!strcmp(attr[i], "properties"))
        properties = attr[i + 1];
    }
    const bool is_image = media_type.find("image/") == 0;
    const bool is_cover_image =
        !properties.empty() && ContainsToken(properties, "cover-image");
    const bool looks_like_cover =
        ContainsNoCase(id, "cover") || ContainsNoCase(href, "cover") ||
        ContainsNoCase(href, "portada");
    const bool keep_manifest_item =
        !d->metadataonly || is_image || is_cover_image || looks_like_cover;

    if (keep_manifest_item) {
      epub_item *item = new epub_item;
      item->id = id;
      item->href = href;
      item->media_type = media_type;
      item->properties = properties;
      d->manifest.push_back(item);
    }

    if (!d->metadataonly && !properties.empty() &&
        ContainsToken(properties, "nav")) {
      d->navid = id;
    }
    if (is_cover_image) {
      d->coverid = id;
    }
    if (!d->metadataonly && d->tocid.empty() &&
        media_type == "application/x-dtbncx+xml") {
      d->tocid = id;
    }
  }

  else if (elem == "spine" || elem == "opf:spine") {
    if (d->metadataonly) {
      d->metadata_parse_complete = true;
      return;
    }
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
  if (d->metadata_parse_complete)
    return;
  if (d->ctx.empty())
    return;
  delete d->ctx.back();
  d->ctx.pop_back();
}

void epub_rootfile_char(void *data, const XML_Char *txt, int len) {
  epub_data_t *d = (epub_data_t *)data;
  if (d->metadata_parse_complete)
    return;
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
  parsedata->coalesce_text_segments = true;
}

int epub_parse_currentfile(unzFile uf, epub_data_t *epd, const EpubDeps &deps,
                           unzFile css_scan_uf) {
  int rc = 0;
  parsedata_t pd;
#if defined(DSLIBRIS_DEBUG) && EPUB_LAYOUT_PROFILE
  bool log_content_layout = false;
  u64 t_content_begin = 0;
  u16 pages_before = 0;
  u32 chardata_calls_before = 0;
  u64 chardata_ms_before = 0;
  u64 element_ms_before = 0;
  u32 element_calls_before = 0;
  u64 flush_ms_before = 0;
  u32 flush_calls_before = 0;
  u32 overflow_before = 0;
  text_layout_utils::PerfStats layout_before;
#endif
  xml_parse_utils::XmlParserOptions options;
  if (epd->type == PARSE_CONTAINER) {
    options.user_data = epd;
    options.abort_parse = [](void *user_data) {
      Book *book = static_cast<Book *>(user_data);
      return book &&
             (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                     "epub-container-parse") ||
              (book->GetStatusReporter() &&
               book->GetStatusReporter()->ShouldAbortWork()) ||
              book->IsOpenAbortRequested());
    };
    options.abort_user_data = epd->book;
    options.start_element = epub_container_start;
  } else if (epd->type == PARSE_ROOTFILE) {
    options.user_data = epd;
    options.abort_parse = [](void *user_data) {
      epub_data_t *data = static_cast<epub_data_t *>(user_data);
      if (!data)
        return false;
      if (data->metadataonly && data->metadata_parse_complete)
        return true;
      Book *book = data->book;
      return book &&
             (open_cancel_poll::Poll(book, book->GetStatusReporter(),
                                     "epub-rootfile-parse") ||
              (book->GetStatusReporter() &&
               book->GetStatusReporter()->ShouldAbortWork()) ||
              book->IsOpenAbortRequested());
    };
    options.abort_user_data = epd;
    options.start_element = epub_rootfile_start;
    options.end_element = epub_rootfile_end;
    options.character_data = epub_rootfile_char;
  } else if (epd->type == PARSE_CONTENT) {
    epd->parsed_doc_title.clear();
    InitParsedataWithEpubDeps(&pd, epd->book, deps);
    pd.docpath = epd->docpath;
    if (!epd->archive_path.empty())
      LoadCssClassMapForDoc(epd->archive_path, epd->docpath, deps.reporter,
                            epd, &pd.css_class_map, css_scan_uf);
    {
      epub_css_class_map::FontSizeSpec body_spec;
      if (epub_css_class_map::LookupFontSizeForTag("body", pd.css_class_map,
                                                    &body_spec) &&
          body_spec.unit == epub_css_class_map::FontSizeSpec::Unit::Px) {
        const int body_px = (body_spec.value_x100 + 50) / 100;
        if (body_px >= 6 && body_px <= 72)
          pd.css_px_baseline = (u8)body_px;
      }
    }
#if defined(DSLIBRIS_DEBUG) && EPUB_LAYOUT_PROFILE
    log_content_layout = deps.reporter && epd->book;
    if (log_content_layout) {
      t_content_begin = osGetTime();
      pages_before = epd->book->GetPageCount();
      chardata_calls_before = pd.perf_chardata_calls;
      chardata_ms_before = pd.perf_chardata_ms;
      element_ms_before = pd.perf_element_ms;
      element_calls_before = pd.perf_element_calls;
      flush_ms_before = pd.perf_flush_ms;
      flush_calls_before = pd.perf_flush_calls;
      overflow_before = pd.perf_page_overflows;
      layout_before = text_layout_utils::GetPerfStats();
    }
#endif
#ifdef DSLIBRIS_DEBUG
    // Always-on snapshot for the spine-wide aggregator. Cheap struct copy.
    const epub_parse_perf::Snapshot perf_before_doc = {
        pd.perf_chardata_ms,    pd.perf_element_ms,    pd.perf_flush_ms,
        pd.perf_chardata_calls, pd.perf_element_calls, pd.perf_flush_calls,
        pd.perf_page_overflows};
#endif
    options.user_data = &pd;
    options.abort_parse = [](void *user_data) {
      parsedata_t *parsedata = static_cast<parsedata_t *>(user_data);
      return parsedata &&
             ((parsedata->book &&
               open_cancel_poll::Poll(parsedata->book, parsedata->reporter,
                                      "epub-content-parse")) ||
              (parsedata->book && parsedata->book->IsOpenAbortRequested()) ||
              (parsedata->reporter && parsedata->reporter->ShouldAbortWork()));
    };
    options.abort_user_data = &pd;
    options.start_element = xml::book::start;
    options.end_element = xml::book::end;
    options.character_data = xml::book::chardata;
    options.default_handler = xml::book::fallback;
    options.processing_instruction = xml::book::instruction;
  } else
    return 0;

  xml_parse_utils::XmlParseResult parse_result;
  if (epd->type == PARSE_CONTENT) {
    html_entity_utils::ChunkedEntityNormalizerState normalize_state;
    parse_result = xml_parse_utils::ParseXmlZipEntryTransformed(
        uf, options, parser_limits::kXmlStreamBufferSize,
        NormalizeHtmlEntityChunkForXml, &normalize_state);
  } else {
    parse_result =
        xml_parse_utils::ParseXmlZipEntry(uf, options,
                                          parser_limits::kXmlStreamBufferSize);
  }
#if defined(DSLIBRIS_DEBUG) && EPUB_LAYOUT_PROFILE
  if (log_content_layout) {
    const text_layout_utils::PerfStats layout_after =
        text_layout_utils::GetPerfStats();
    DBG_LOGF(deps.reporter,
             "EPUB layout: stream=%llums elem=%llums/%u flush=%llums/%u "
             "chardata=%llums/%u "
             "shape=%llums/%u break=%llums/%u pre=%llums/%u "
             "measure=%llums/%u glyphs=%u pages=%u overflow_pages=%u path=%s",
             (unsigned long long)(osGetTime() - t_content_begin),
             (unsigned long long)(pd.perf_element_ms - element_ms_before),
             (unsigned)(pd.perf_element_calls - element_calls_before),
             (unsigned long long)(pd.perf_flush_ms - flush_ms_before),
             (unsigned)(pd.perf_flush_calls - flush_calls_before),
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
#ifdef DSLIBRIS_DEBUG
  if (epd->type == PARSE_CONTENT)
    epub_parse_perf::Add(pd, perf_before_doc);
#endif
  if (!parse_result.ok)
    rc = (parse_result.error_code == XML_ERROR_ABORTED)
             ? ((epd->type == PARSE_ROOTFILE && epd->metadataonly &&
                 epd->metadata_parse_complete)
                    ? 0
                    : BOOK_ERR_CANCELLED)
             : (int)parse_result.error_code;
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
    bool metadataonly, const EpubDeps &deps
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
  if (rc != UNZ_OK)
    return rc;
  epub_data_init(parsedata);
  parsedata->book = book;
  parsedata->type = PARSE_CONTAINER;
  {
    const int parse_rc = epub_parse_currentfile(uf, parsedata, deps);
    const int close_rc = unzCloseCurrentFile(uf);
    rc = (parse_rc != 0) ? parse_rc : close_rc;
  }
#ifdef DSLIBRIS_DEBUG
  if (t_after_container)
    *t_after_container = osGetTime();
#endif
  if (rc != 0)
    return rc;

  folder->clear();
  size_t pos = parsedata->rootfile.find_last_of("/", parsedata->rootfile.length());
  if (pos < parsedata->rootfile.length())
    *folder = parsedata->rootfile.substr(0, pos);

  rc = unzLocateFile(uf, parsedata->rootfile.c_str(), 0);
  if (rc == UNZ_OK) {
    rc = unzOpenCurrentFile(uf);
    if (rc != UNZ_OK)
      return rc;
    epub_data_init(parsedata);
    parsedata->book = book;
    parsedata->type = PARSE_ROOTFILE;
    parsedata->metadataonly = metadataonly;
    const int parse_rc = epub_parse_currentfile(uf, parsedata, deps);
    const int close_rc = unzCloseCurrentFile(uf);
    rc = (parse_rc != 0) ? parse_rc : close_rc;
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
