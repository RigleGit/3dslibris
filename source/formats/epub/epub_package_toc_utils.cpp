/*
    3dslibris - epub_package_toc_utils.cpp

    EPUB package and TOC proxy utility helpers extracted from epub.cpp.
*/

#include "formats/epub/epub_package_toc_utils.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "debug_log.h"
#include "formats/common/xml_parse_utils.h"
#include "formats/epub/epub_limits.h"
#include "path_utils.h"
#include "shared/status_reporter.h"
#include "string_utils.h"

namespace {

static const char *AttrValue(const char **attr, const char *name) {
  if (!attr)
    return nullptr;
  size_t name_len = strlen(name);
  for (int i = 0; attr[i]; i += 2) {
    const char *key = attr[i];
    if (!strcmp(key, name))
      return attr[i + 1];
    size_t key_len = strlen(key);
    if (key_len > name_len + 1 && key[key_len - name_len - 1] == ':' &&
        !strcmp(key + key_len - name_len, name)) {
      return attr[i + 1];
    }
  }
  return nullptr;
}

typedef struct {
  std::map<std::string, std::string> *fragment_to_href;
  std::string base_path;
  std::vector<std::string> id_stack;
  std::vector<bool> pushed_stack;
  std::vector<std::string> pending_ids;
} toc_proxy_parse_data_t;

static std::string DigitsOnly(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= '0' && c <= '9')
      out.push_back((char)c);
  }
  return out;
}

static void toc_proxy_start(void *userdata, const char *el, const char **attr) {
  (void)el;
  toc_proxy_parse_data_t *d = (toc_proxy_parse_data_t *)userdata;
  bool pushed = false;

  std::string current_id;
  const char *id = AttrValue(attr, "id");
  if (!id || !*id)
    id = AttrValue(attr, "name");
  if (id && *id) {
    current_id = epub_package_toc_utils::NormalizeFragmentId(id);
    if (!current_id.empty()) {
      d->id_stack.push_back(current_id);
      pushed = true;
    }
  }

  const char *href = AttrValue(attr, "href");
  if (!href || !*href)
    href = AttrValue(attr, "src");
  if (href && *href) {
    std::string resolved = ResolveRelativePath(d->base_path, href);
    if (!resolved.empty()) {
      bool mapped = false;
      if (!current_id.empty()) {
        if (d->fragment_to_href->find(current_id) ==
            d->fragment_to_href->end()) {
          (*d->fragment_to_href)[current_id] = resolved;
        }
        mapped = true;
      } else if (!d->id_stack.empty()) {
        const std::string &id_from_parent = d->id_stack.back();
        if (d->fragment_to_href->find(id_from_parent) ==
            d->fragment_to_href->end()) {
          (*d->fragment_to_href)[id_from_parent] = resolved;
        }
        mapped = true;
      }

      if (!mapped && !d->pending_ids.empty()) {
        std::string id_from_pending = d->pending_ids.back();
        d->pending_ids.pop_back();
        if (!id_from_pending.empty() &&
            d->fragment_to_href->find(id_from_pending) ==
                d->fragment_to_href->end()) {
          (*d->fragment_to_href)[id_from_pending] = resolved;
        }
      }
    }
  } else if (!current_id.empty()) {
    d->pending_ids.push_back(current_id);
  }

  d->pushed_stack.push_back(pushed);
}

static void toc_proxy_end(void *userdata, const char *el) {
  (void)el;
  toc_proxy_parse_data_t *d = (toc_proxy_parse_data_t *)userdata;
  if (d->pushed_stack.empty())
    return;
  bool pushed = d->pushed_stack.back();
  d->pushed_stack.pop_back();
  if (pushed && !d->id_stack.empty())
    d->id_stack.pop_back();
}

static void
BuildTocProxyMapFromHtmlScan(const std::string &xml,
                             const std::string &base_path,
                             std::map<std::string, std::string> *proxy_map) {
  if (!proxy_map)
    return;

  std::vector<std::string> pending_ids;
  size_t pos = 0;
  while (pos < xml.size()) {
    size_t lt = xml.find('<', pos);
    if (lt == std::string::npos)
      break;
    size_t gt = xml.find('>', lt + 1);
    if (gt == std::string::npos)
      break;
    pos = gt + 1;

    std::string tag = Trim(xml.substr(lt + 1, gt - lt - 1));
    if (tag.empty())
      continue;
    if (tag[0] == '!' || tag[0] == '?' || tag[0] == '/')
      continue;

    size_t name_end = 0;
    while (name_end < tag.size() && !isspace((unsigned char)tag[name_end]) &&
           tag[name_end] != '/')
      name_end++;
    std::string name = tag.substr(0, name_end);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)tolower(c); });

    // Collect IDs from any tag; in malformed TOC docs the id may be attached to
    // li/p/tr elements while href appears in nested <a>.
    std::string id =
        epub_package_toc_utils::NormalizeFragmentId(ExtractHtmlAttrValue(tag, "id"));
    if (id.empty())
      id = epub_package_toc_utils::NormalizeFragmentId(
          ExtractHtmlAttrValue(tag, "name"));
    if (id.empty()) {
      // Namespace variants like xml:id, epub:id...
      size_t pos = 0;
      while (pos < tag.size()) {
        while (pos < tag.size() && isspace((unsigned char)tag[pos]))
          pos++;
        size_t k0 = pos;
        while (pos < tag.size() && IsHtmlNameChar((unsigned char)tag[pos]))
          pos++;
        if (pos == k0) {
          pos++;
          continue;
        }
        std::string key = tag.substr(k0, pos - k0);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        if (key.size() > 3 && key.substr(key.size() - 3) == ":id") {
          size_t eq = tag.find('=', pos);
          if (eq == std::string::npos)
            break;
          std::string tail = tag.substr(k0);
          std::string val = ExtractHtmlAttrValue(tail, key);
          id = epub_package_toc_utils::NormalizeFragmentId(val);
          break;
        }
      }
    }

    std::string href = ExtractHtmlAttrValue(tag, "href");
    if (href.empty())
      href = ExtractHtmlAttrValue(tag, "src");
    if (href.empty()) {
      std::string ns_href = ExtractHtmlAttrValue(tag, "xlink:href");
      if (!ns_href.empty())
        href = ns_href;
    }

    if (!id.empty())
      pending_ids.push_back(id);

    if (href.empty())
      continue;

    std::string resolved = ResolveRelativePath(base_path, href);
    if (resolved.empty())
      continue;

    if (!id.empty() && proxy_map->find(id) == proxy_map->end())
      (*proxy_map)[id] = resolved;

    if (!pending_ids.empty()) {
      std::string pid = pending_ids.back();
      pending_ids.pop_back();
      if (!pid.empty() && proxy_map->find(pid) == proxy_map->end())
        (*proxy_map)[pid] = resolved;
    }
  }
}

static void EpubDiag(IStatusReporter *reporter, const char *fmt,
                     const char *arg = NULL) {
  if (!reporter || !fmt)
    return;
  char msg[192];
  if (arg)
    snprintf(msg, sizeof(msg), fmt, arg);
  else
    snprintf(msg, sizeof(msg), "%s", fmt);
  DBG_LOG(reporter, msg);
}

static bool BuildTocFragmentProxyMap(
    unzFile uf, const std::string &doc_path,
    std::map<std::string, std::string> *proxy_map, IStatusReporter *reporter) {
  if (!proxy_map || doc_path.empty())
    return false;
  proxy_map->clear();

  std::string xml;
  if (!epub_package_toc_utils::ReadZipEntryText(uf, doc_path, xml, reporter,
                                                "TOC-PROXY"))
    return false;

  toc_proxy_parse_data_t data;
  data.fragment_to_href = proxy_map;
  data.base_path = doc_path;
  data.id_stack.clear();
  data.pushed_stack.clear();

  xml_parse_utils::XmlParserOptions options;
  options.start_element = toc_proxy_start;
  options.end_element = toc_proxy_end;
  options.user_data = &data;
  if (!xml_parse_utils::ParseXmlString(xml, options).ok)
    return false;

  if (proxy_map->size() < 4) {
    // Some generated index_split files are not well-formed XML; salvage
    // id->href mapping with a tolerant tag scanner.
    BuildTocProxyMapFromHtmlScan(xml, doc_path, proxy_map);
  }

  if (reporter) {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: TOC-PROXY map size=%u",
             (unsigned)proxy_map->size());
    DBG_LOG(reporter, msg);
  }

  return !proxy_map->empty();
}

} // namespace

namespace epub_package_toc_utils {

std::string NormalizeFragmentId(const std::string &raw) {
  std::string frag = Trim(UrlDecode(raw));
  while (!frag.empty() && frag[0] == '#')
    frag.erase(frag.begin());
  size_t q = frag.find('?');
  if (q != std::string::npos)
    frag = frag.substr(0, q);
  return frag;
}

std::string ExtractHrefFragment(const std::string &href) {
  size_t hash = href.find('#');
  if (hash == std::string::npos || hash + 1 >= href.size())
    return "";
  return NormalizeFragmentId(href.substr(hash + 1));
}

bool LocateZipEntrySafe(unzFile uf, const std::string &entry_path,
                        IStatusReporter *reporter, const char *tag,
                        epub_zip_utils::ZipEntryIndex *index) {
  if (!uf || entry_path.empty())
    return false;
  const char *t = (tag && *tag) ? tag : "ZIP";
  bool ok = epub_zip_utils::LocateSafe(uf, entry_path, index);
  if (!ok && reporter) {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: %s locate miss path=%s", t,
             entry_path.c_str());
    DBG_LOG(reporter, msg);
  }
  return ok;
}

bool ReadZipEntryText(unzFile uf, const std::string &path, std::string &out,
                      IStatusReporter *reporter, const char *tag,
                      epub_zip_utils::ZipEntryIndex *index) {
  out.clear();
  const char *t = (tag && *tag) ? tag : "ZIP";
  {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: %s read begin %s", t, path.c_str());
    EpubDiag(reporter, msg);
  }
  if (path.empty())
    return false;
  EpubDiag(reporter, "EPUB: %s locate begin", t);
  if (!epub_zip_utils::LocateSafe(uf, path, index)) {
    EpubDiag(reporter, "EPUB: %s locate fail", t);
    return false;
  }
  EpubDiag(reporter, "EPUB: %s locate ok", t);
  bool ok =
      epub_zip_utils::ReadText(uf, path, out, epub_limits::kTocMaxBytes, index);
  if (!ok) {
    EpubDiag(reporter, "EPUB: %s read fail", t);
    return false;
  }
  {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: %s read ok bytes=%u", t,
             (unsigned)out.size());
    EpubDiag(reporter, msg);
  }
  return true;
}

bool LookupTocProxyHref(
    unzFile uf, const std::string &doc_path, const std::string &fragment_raw,
    std::map<std::string, std::map<std::string, std::string>> *cache,
    std::set<std::string> *attempted, std::string *href_out,
    IStatusReporter *reporter) {
  if (!cache || !attempted || !href_out || doc_path.empty())
    return false;
  href_out->clear();

  std::string fragment = NormalizeFragmentId(fragment_raw);
  if (fragment.empty())
    return false;

  if (attempted->find(doc_path) == attempted->end()) {
    std::map<std::string, std::string> local_map;
    BuildTocFragmentProxyMap(uf, doc_path, &local_map, reporter);
    (*cache)[doc_path] = local_map;
    attempted->insert(doc_path);
  }

  auto hit_doc = cache->find(doc_path);
  if (hit_doc == cache->end())
    return false;
  auto hit = hit_doc->second.find(fragment);
  if (hit != hit_doc->second.end()) {
    *href_out = hit->second;
    return !href_out->empty();
  }

  // Some generated TOCs use inconsistent fragment identifiers; if both carry
  // stable numeric suffixes, allow a digit-only match.
  std::string fragment_digits = DigitsOnly(fragment);
  if (fragment_digits.size() < 1)
    return false;

  std::string fallback_href;
  bool found = false;
  for (const auto &kv : hit_doc->second) {
    if (DigitsOnly(kv.first) != fragment_digits)
      continue;
    if (!found) {
      fallback_href = kv.second;
      found = true;
    } else if (fallback_href != kv.second) {
      found = false;
      break;
    }
  }
  if (!found)
    return false;

  *href_out = fallback_href;
  return !href_out->empty();
}

std::string BuildDocPath(const std::string &opf_folder, const std::string &href) {
  if (opf_folder.empty())
    return NormalizePath(UrlDecode(href));
  return NormalizePath(opf_folder + "/" + UrlDecode(href));
}

bool FindManifestItemPath(epub_data_t &data, const std::string &id,
                          const std::string &opf_folder,
                          std::string &path_out) {
  for (auto item : data.manifest) {
    if (item->id == id) {
      path_out = BuildDocPath(opf_folder, item->href);
      return true;
    }
  }
  return false;
}

} // namespace epub_package_toc_utils
