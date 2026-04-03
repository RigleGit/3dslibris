/*
    3dslibris - epub_toc_package_loader_utils.cpp

    EPUB package TOC loading utilities extracted from epub.cpp.
*/

#include "formats/epub/epub_toc_package_loader_utils.h"

#include <set>
#include <unordered_map>

#include "book/book.h"
#include "debug_log.h"
#include "formats/epub/epub_ncx_parser.h"
#include "formats/epub/epub_package_toc_utils.h"
#include "formats/epub/epub_toc_diag_utils.h"
#include "path_utils.h"
#include "shared/status_reporter.h"
#include "string_utils.h"

namespace {

using epub_package_toc_utils::BuildDocPath;
using epub_package_toc_utils::FindManifestItemPath;
using epub_package_toc_utils::ReadZipEntryText;
using epub_toc_diag_utils::LogTocEntrySamples;

static const epub_item *FindManifestItemById(const epub_data_t &data,
                                             const std::string &id) {
  for (auto item : data.manifest) {
    if (item && item->id == id)
      return item;
  }
  return NULL;
}

static bool IsLikelyContentItem(const epub_item *item) {
  if (!item)
    return false;
  std::string media = item->media_type;
  std::transform(media.begin(), media.end(), media.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  if (media == "application/xhtml+xml" || media == "text/html")
    return true;
  std::string href = item->href;
  std::transform(href.begin(), href.end(), href.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return (href.size() >= 6 && href.substr(href.size() - 6) == ".xhtml") ||
         (href.size() >= 5 && href.substr(href.size() - 5) == ".html") ||
         (href.size() >= 4 && href.substr(href.size() - 4) == ".htm");
}

} // namespace

namespace epub_toc_package_loader_utils {

void BuildPageStartMapFromPackage(const epub_data_t &parsedata,
                                  const std::string &opf_folder, Book *book,
                                  std::map<std::string, u16> *out) {
  if (!book || !out)
    return;
  out->clear();

  const auto &cached = book->GetChapterDocStartPages();
  if (!cached.empty()) {
    for (const auto &kv : cached)
      (*out)[kv.first] = kv.second;
    return;
  }

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.empty())
    return;

  size_t chapter_index = 0;
  auto map_item = [&](const epub_item *item) {
    if (!item || item->href.empty())
      return;
    if (chapter_index >= chapters.size())
      return;
    std::string key = NormalizePath(BuildDocPath(opf_folder, item->href));
    if (key.empty())
      return;
    if (out->find(key) == out->end()) {
      (*out)[key] = chapters[chapter_index].page;
      chapter_index++;
    }
  };

  if (!parsedata.spine.empty()) {
    for (auto itemref : parsedata.spine) {
      if (chapter_index >= chapters.size())
        break;
      if (!itemref)
        continue;
      map_item(FindManifestItemById(parsedata, itemref->idref));
    }
  }

  if (out->empty()) {
    for (auto item : parsedata.manifest) {
      if (chapter_index >= chapters.size())
        break;
      if (!IsLikelyContentItem(item))
        continue;
      map_item(item);
    }
  }

  if (out->empty()) {
    for (auto item : parsedata.manifest) {
      if (chapter_index >= chapters.size())
        break;
      map_item(item);
    }
  }
}

bool LoadTocEntriesFromPackage(unzFile uf, epub_data_t &parsedata,
                               const std::string &opf_folder,
                               std::vector<toc_entry_t> *toc_entries,
                               IStatusReporter *reporter) {
  if (!uf || !toc_entries)
    return false;
  IStatusReporter *app = reporter;

  toc_entries->clear();
  std::string toc_xml;
  std::string toc_doc_path;
  bool toc_loaded = false;

  auto looks_reasonable_toc = [&](const std::vector<toc_entry_t> &entries) {
    if (entries.empty())
      return false;
    if (entries.size() == 1) {
      // When a NAV document exists, a 1-item NCX is usually low quality
      // (common in mixed EPUB2/EPUB3 packages). Force fallback NAV probing.
      bool has_nav_candidate = !parsedata.navid.empty();
      if (!has_nav_candidate) {
        for (auto item : parsedata.manifest) {
          if (!item)
            continue;
          if (item->media_type == "application/xhtml+xml" &&
              ContainsToken(item->properties, "nav")) {
            has_nav_candidate = true;
            break;
          }
        }
      }
      if (has_nav_candidate)
        return false;
      return entries[0].title.size() <= 120 && !entries[0].href.empty();
    }

    size_t too_long = 0;
    size_t empty_href = 0;
    size_t with_fragment = 0;
    std::set<std::string> unique_hrefs;
    for (const auto &e : entries) {
      if (e.title.size() > 140)
        too_long++;
      if (e.href.empty())
        empty_href++;
      else {
        unique_hrefs.insert(StripFragmentAndQuery(e.href));
        if (e.href.find('#') != std::string::npos)
          with_fragment++;
      }
    }

    if (too_long * 4 > entries.size())
      return false; // >25% suspiciously long labels
    if (empty_href * 2 > entries.size())
      return false; // >50% without href
    // Allow single-document TOCs (common in old EPUBs) when entries are mostly
    // in-document anchors (chapter.xhtml#id).
    if (entries.size() >= 6 && unique_hrefs.size() < 2 &&
        with_fragment * 3 < entries.size() * 2)
      return false;
    return true;
  };

  if (!parsedata.navid.empty() &&
      FindManifestItemPath(parsedata, parsedata.navid, opf_folder,
                           toc_doc_path)) {
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: try NAV %s", toc_doc_path.c_str());
      DBG_LOG(app, msg);
    }
    if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NAV")) {
      std::vector<toc_entry_t> nav_entries;
      bool parsed_nav =
          epub_ncx_parser::ParseNavWithExpat(toc_xml, toc_doc_path, &nav_entries);
      if (parsed_nav && app)
        LogTocEntrySamples(app, "NAV parsed", nav_entries, 4);
      if (parsed_nav && looks_reasonable_toc(nav_entries)) {
        *toc_entries = nav_entries;
        toc_loaded = true;
        if (app)
          LogTocEntrySamples(app, "NAV selected", *toc_entries, 4);
      } else if (app) {
        DBG_LOG(app, "EPUB: NAV discarded (low-quality TOC)");
      }
    } else if (app) {
      DBG_LOG(app, "EPUB: NAV skipped (size/format)");
    }
  }

  if (!toc_loaded && !parsedata.tocid.empty() &&
      FindManifestItemPath(parsedata, parsedata.tocid, opf_folder,
                           toc_doc_path)) {
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: try NCX %s", toc_doc_path.c_str());
      DBG_LOG(app, msg);
    }
    if (app)
      DBG_LOG(app, "EPUB: NCX read call begin");
    bool ncx_read_ok = ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NCX");
    if (app) {
      DBG_LOG(app, ncx_read_ok ? "EPUB: NCX read call ok"
                               : "EPUB: NCX read call fail");
    }
    if (ncx_read_ok) {
      std::vector<toc_entry_t> ncx_entries;
      bool parsed =
          epub_ncx_parser::ParseNcxWithExpat(toc_xml, toc_doc_path, &ncx_entries);
      if (!parsed)
        parsed = epub_ncx_parser::ParseNcxLightweight(toc_xml, toc_doc_path,
                                                      &ncx_entries);
      if (parsed && app)
        LogTocEntrySamples(app, "NCX parsed", ncx_entries, 4);
      if (parsed && looks_reasonable_toc(ncx_entries)) {
        *toc_entries = ncx_entries;
        toc_loaded = true;
        if (app)
          LogTocEntrySamples(app, "NCX selected", *toc_entries, 4);
      } else if (app) {
        DBG_LOG(app, "EPUB: NCX discarded (low-quality TOC)");
      }
    } else if (app) {
      DBG_LOG(app, "EPUB: NCX skipped (size/format)");
    }
  }

  if (!toc_loaded) {
    for (auto item : parsedata.manifest) {
      if (!item)
        continue;
      if (item->media_type == "application/xhtml+xml" &&
          ContainsToken(item->properties, "nav")) {
        toc_doc_path = BuildDocPath(opf_folder, item->href);
        if (app) {
          char msg[192];
          snprintf(msg, sizeof(msg), "EPUB: fallback NAV %s",
                   toc_doc_path.c_str());
          DBG_LOG(app, msg);
        }
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NAV-FALLBACK")) {
          std::vector<toc_entry_t> nav_entries;
          bool parsed_nav = epub_ncx_parser::ParseNavWithExpat(
              toc_xml, toc_doc_path, &nav_entries);
          if (parsed_nav && app)
            LogTocEntrySamples(app, "NAV fallback parsed", nav_entries, 4);
          if (parsed_nav && looks_reasonable_toc(nav_entries)) {
            *toc_entries = nav_entries;
            toc_loaded = true;
            if (app)
              LogTocEntrySamples(app, "NAV fallback selected", *toc_entries, 4);
          }
        } else if (app) {
          DBG_LOG(app, "EPUB: fallback NAV skipped");
        }
        if (toc_loaded)
          break;
      }
    }
  }

  if (!toc_loaded) {
    for (auto item : parsedata.manifest) {
      if (!item)
        continue;
      if (item->media_type == "application/x-dtbncx+xml") {
        toc_doc_path = BuildDocPath(opf_folder, item->href);
        if (app) {
          char msg[192];
          snprintf(msg, sizeof(msg), "EPUB: fallback NCX %s",
                   toc_doc_path.c_str());
          DBG_LOG(app, msg);
        }
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NCX-FALLBACK")) {
          std::vector<toc_entry_t> ncx_entries;
          bool parsed =
              epub_ncx_parser::ParseNcxWithExpat(toc_xml, toc_doc_path, &ncx_entries);
          if (!parsed)
            parsed = epub_ncx_parser::ParseNcxLightweight(toc_xml, toc_doc_path,
                                                          &ncx_entries);
          if (parsed && app)
            LogTocEntrySamples(app, "NCX fallback parsed", ncx_entries, 4);
          if (parsed && looks_reasonable_toc(ncx_entries)) {
            *toc_entries = ncx_entries;
            toc_loaded = true;
            if (app)
              LogTocEntrySamples(app, "NCX fallback selected", *toc_entries, 4);
          }
        } else if (app) {
          DBG_LOG(app, "EPUB: fallback NCX skipped");
        }
        if (toc_loaded)
          break;
      }
    }
  }

  return !toc_entries->empty();
}

} // namespace epub_toc_package_loader_utils

