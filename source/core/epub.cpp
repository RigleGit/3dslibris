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

#include "epub.h"

#include "book.h"
#include "expat.h"
#include "main.h"
#include "parse.h"
#include "stb_image.h"
#include "unzip.h"
#include "zlib.h"
#include <3ds.h>
#include <algorithm>
#include <ctype.h>
#include <map>
#include <set>
#include <stdio.h>
#include <string.h>
#include <vector>

static const size_t EPUB_TOC_MAX_BYTES = 192 * 1024;
static const size_t EPUB_TOC_MAX_ENTRIES = 2048;
// Safety switch for 3DS stability: TOC/NAV title resolution can be re-enabled
// once NCX/NAV parsing is fully hardened on real hardware/emulators.
static const bool EPUB_ENABLE_REAL_TOC_RESOLVE = false;
static std::string Trim(const std::string &s);

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
    bool is_space = isspace(uc) || title[i] == '\n' || title[i] == '\r' ||
                    title[i] == '\t';
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

static std::string Trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && isspace((unsigned char)s[start]))
    start++;
  size_t end = s.size();
  while (end > start && isspace((unsigned char)s[end - 1]))
    end--;
  return s.substr(start, end - start);
}

static std::string UrlDecode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int value = 0;
      if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
        out.push_back((char)value);
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

static std::string NormalizePath(const std::string &path) {
  std::string in = path;
  std::replace(in.begin(), in.end(), '\\', '/');
  while (!in.empty() && in[0] == '/')
    in.erase(in.begin());

  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= in.size(); i++) {
    if (i == in.size() || in[i] == '/') {
      if (cur == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(in[i]);
    }
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i)
      out.push_back('/');
    out += parts[i];
  }
  return out;
}

static std::string StripFragmentAndQuery(const std::string &path) {
  size_t stop = path.find_first_of("#?");
  if (stop == std::string::npos)
    return path;
  return path.substr(0, stop);
}

static std::string ResolveRelativePath(const std::string &base_file,
                                       const std::string &ref_raw) {
  // Keep fragment for TOC identity (anchors) and strip it only when needed for
  // physical file lookups.
  std::string ref = UrlDecode(ref_raw);
  if (ref.empty())
    return "";
  if (ref.find("://") != std::string::npos)
    return "";

  if (ref[0] == '/') {
    return NormalizePath(ref);
  }

  std::string base = base_file;
  size_t slash = base.find_last_of('/');
  std::string folder = (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
  return NormalizePath(folder + ref);
}

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

static const char *LocalName(const char *name) {
  const char *c = strrchr(name, ':');
  return c ? c + 1 : name;
}

typedef struct {
  std::string href;
  std::string title;
} toc_entry_t;

typedef struct {
  std::vector<toc_entry_t> *entries;
  std::string base_path;
  int depth;
  int toc_nav_depth;
  int anchor_depth;
  std::string current_href;
  std::string current_title;
} nav_parse_data_t;

static void EpubDiag(App *app, const char *fmt, const char *arg = NULL);

static void nav_start(void *userdata, const char *el, const char **attr) {
  nav_parse_data_t *d = (nav_parse_data_t *)userdata;
  d->depth++;
  const char *lname = LocalName(el);

  if (!strcmp(lname, "nav")) {
    const char *type = AttrValue(attr, "type");
    if (type && strstr(type, "toc"))
      d->toc_nav_depth = d->depth;
  }

  if (d->toc_nav_depth > 0 && !strcmp(lname, "a")) {
    const char *href = AttrValue(attr, "href");
    if (href && *href) {
      d->anchor_depth = d->depth;
      d->current_href = ResolveRelativePath(d->base_path, href);
      d->current_title.clear();
    }
  }
}

static void nav_char(void *userdata, const XML_Char *txt, int len) {
  nav_parse_data_t *d = (nav_parse_data_t *)userdata;
  if (d->anchor_depth > 0) {
    d->current_title.append((const char *)txt, len);
  }
}

static void nav_end(void *userdata, const char *el) {
  nav_parse_data_t *d = (nav_parse_data_t *)userdata;
  const char *lname = LocalName(el);

  if (d->anchor_depth == d->depth && !strcmp(lname, "a")) {
    std::string title = Trim(d->current_title);
    if (!d->current_href.empty() && !title.empty()) {
      if (d->entries->size() < EPUB_TOC_MAX_ENTRIES) {
        toc_entry_t entry;
        entry.href = d->current_href;
        entry.title = title;
        d->entries->push_back(entry);
      }
    }
    d->anchor_depth = 0;
    d->current_href.clear();
    d->current_title.clear();
  }

  if (d->toc_nav_depth == d->depth && !strcmp(lname, "nav")) {
    d->toc_nav_depth = 0;
  }

  d->depth--;
}

static bool ParseNcxLightweight(const std::string &xml,
                                const std::string &base_path,
                                std::vector<toc_entry_t> *entries,
                                App *app = NULL) {
  EpubDiag(app, "EPUB: NCX parse begin");
  if (xml.empty() || !entries)
    return false;
  if (xml.size() > EPUB_TOC_MAX_BYTES)
    return false;

  std::string lower = xml;
  for (size_t i = 0; i < lower.size(); i++) {
    lower[i] = (char)tolower((unsigned char)lower[i]);
  }

  std::string pending_src;
  std::string pending_title;
  bool any = false;
  size_t pos = 0;

  auto try_flush = [&]() {
    if (pending_src.empty() || pending_title.empty())
      return;
    if (entries->size() >= EPUB_TOC_MAX_ENTRIES)
      return;
    toc_entry_t entry;
    entry.href = ResolveRelativePath(base_path, pending_src);
    entry.title = Trim(pending_title);
    if (!entry.href.empty() && !entry.title.empty()) {
      entries->push_back(entry);
      any = true;
    }
    pending_src.clear();
    pending_title.clear();
  };

  while (pos < lower.size() && entries->size() < EPUB_TOC_MAX_ENTRIES) {
    size_t content_pos = lower.find("<content", pos);
    size_t text_pos = lower.find("<text", pos);
    if (content_pos == std::string::npos && text_pos == std::string::npos)
      break;

    if (content_pos != std::string::npos &&
        (text_pos == std::string::npos || content_pos < text_pos)) {
      size_t tag_end = lower.find('>', content_pos);
      if (tag_end == std::string::npos)
        break;
      size_t src_pos = lower.find("src", content_pos);
      if (src_pos != std::string::npos && src_pos < tag_end) {
        size_t eq = lower.find('=', src_pos);
        if (eq != std::string::npos && eq < tag_end) {
          size_t v = eq + 1;
          while (v < tag_end && isspace((unsigned char)lower[v]))
            v++;
          if (v < tag_end) {
            char quote = xml[v];
            if (quote == '"' || quote == '\'') {
              size_t v_end = xml.find(quote, v + 1);
              if (v_end != std::string::npos && v_end < tag_end) {
                pending_src = xml.substr(v + 1, v_end - (v + 1));
              }
            } else {
              size_t v_end = v;
              while (v_end < tag_end && !isspace((unsigned char)lower[v_end]) &&
                     lower[v_end] != '>')
                v_end++;
              pending_src = xml.substr(v, v_end - v);
            }
          }
        }
      }
      pos = tag_end + 1;
      try_flush();
      continue;
    }

    size_t tag_end = lower.find('>', text_pos);
    if (tag_end == std::string::npos)
      break;
    size_t close_pos = lower.find("</text>", tag_end + 1);
    if (close_pos == std::string::npos)
      break;
    pending_title = xml.substr(tag_end + 1, close_pos - (tag_end + 1));
    pos = close_pos + 7;
    try_flush();
  }

  {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: NCX parse end entries=%u any=%u",
             (unsigned)entries->size(), any ? 1u : 0u);
    EpubDiag(app, msg);
  }
  return any;
}

static bool ParseXmlBuffer(const std::string &xml, XML_StartElementHandler start,
                           XML_EndElementHandler end,
                           XML_CharacterDataHandler chardata, void *userdata) {
  if (xml.size() > EPUB_TOC_MAX_BYTES)
    return false;
  XML_Parser p = XML_ParserCreate(NULL);
  if (!p)
    return false;
  XML_SetUserData(p, userdata);
  XML_SetElementHandler(p, start, end);
  XML_SetCharacterDataHandler(p, chardata);
  bool ok = XML_Parse(p, xml.c_str(), (int)xml.size(), 1) != XML_STATUS_ERROR;
  XML_ParserFree(p);
  return ok;
}

static void EpubDiag(App *app, const char *fmt, const char *arg) {
  if (!app || !fmt)
    return;
  char msg[192];
  if (arg)
    snprintf(msg, sizeof(msg), fmt, arg);
  else
    snprintf(msg, sizeof(msg), "%s", fmt);
  app->PrintStatus(msg);
}

static bool ReadZipEntryText(unzFile uf, const std::string &path, std::string &out,
                             App *app = NULL, const char *tag = NULL) {
  auto normalize_zip_name = [](const std::string &name) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    return n;
  };

  auto equals_ascii_nocase = [](const std::string &a, const std::string &b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); i++) {
      unsigned char ca = (unsigned char)a[i];
      unsigned char cb = (unsigned char)b[i];
      if (ca >= 'A' && ca <= 'Z')
        ca = (unsigned char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z')
        cb = (unsigned char)(cb - 'A' + 'a');
      if (ca != cb)
        return false;
    }
    return true;
  };

  auto locate_zip_entry_safe = [&](const std::string &entry_path) -> bool {
    if (entry_path.empty())
      return false;

    // Fast path: exact match (avoids minizip case-insensitive compare quirks
    // with non-ASCII filenames inside some EPUB archives).
    if (unzLocateFile(uf, entry_path.c_str(), 0) == UNZ_OK)
      return true;

    // Safe fallback: manual iteration with ASCII-only case folding.
    if (unzGoToFirstFile(uf) != UNZ_OK)
      return false;

    std::string wanted = normalize_zip_name(entry_path);
    do {
      unz_file_info fi;
      char fname[1024];
      if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL,
                                0) != UNZ_OK) {
        continue;
      }
      std::string current = normalize_zip_name(std::string(fname));
      if (current == wanted || equals_ascii_nocase(current, wanted))
        return true;
    } while (unzGoToNextFile(uf) == UNZ_OK);

    return false;
  };

  out.clear();
  const char *t = (tag && *tag) ? tag : "ZIP";
  {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: %s read begin %s", t, path.c_str());
    EpubDiag(app, msg);
  }
  if (path.empty())
    return false;
  EpubDiag(app, "EPUB: %s locate begin", t);
  if (!locate_zip_entry_safe(path)) {
    EpubDiag(app, "EPUB: %s locate fail", t);
    return false;
  }
  EpubDiag(app, "EPUB: %s locate ok", t);
  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK) {
    if (fi.uncompressed_size > EPUB_TOC_MAX_BYTES) {
      EpubDiag(app, "EPUB: %s too big", t);
      return false;
    }
  }
  EpubDiag(app, "EPUB: %s open begin", t);
  if (unzOpenCurrentFile(uf) != UNZ_OK) {
    EpubDiag(app, "EPUB: %s open fail", t);
    return false;
  }
  EpubDiag(app, "EPUB: %s open ok", t);

  char buf[BUFSIZE];
  int n = 0;
  EpubDiag(app, "EPUB: %s read loop", t);
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    if (out.size() + (size_t)n > EPUB_TOC_MAX_BYTES) {
      unzCloseCurrentFile(uf);
      out.clear();
      EpubDiag(app, "EPUB: %s read overflow", t);
      return false;
    }
    out.append(buf, n);
  }
  EpubDiag(app, "EPUB: %s close begin", t);
  unzCloseCurrentFile(uf);
  EpubDiag(app, "EPUB: %s close ok", t);
  {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: %s read done bytes=%u", t,
             (unsigned)out.size());
    EpubDiag(app, msg);
  }
  return n >= 0;
}

static std::string BuildDocPath(const std::string &opf_folder,
                                const std::string &href) {
  if (opf_folder.empty())
    return NormalizePath(UrlDecode(href));
  return NormalizePath(opf_folder + "/" + UrlDecode(href));
}

static bool ContainsNoCase(const std::string &haystack,
                           const std::string &needle) {
  if (needle.empty())
    return true;
  if (haystack.empty())
    return false;
  std::string h = haystack;
  std::string n = needle;
  std::transform(h.begin(), h.end(), h.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  std::transform(n.begin(), n.end(), n.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return h.find(n) != std::string::npos;
}

static bool ContainsToken(const std::string &list, const std::string &token) {
  size_t start = 0;
  while (start < list.size()) {
    while (start < list.size() && isspace((unsigned char)list[start]))
      start++;
    size_t end = start;
    while (end < list.size() && !isspace((unsigned char)list[end]))
      end++;
    if (end > start && list.substr(start, end - start) == token)
      return true;
    start = end;
  }
  return false;
}

static bool FindManifestItemPath(epub_data_t &data, const std::string &id,
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

static bool FindLikelyCoverImagePath(epub_data_t &data,
                                     const std::string &opf_folder,
                                     std::string &path_out) {
  // 1) Standard explicit cover-id.
  if (!data.coverid.empty() &&
      FindManifestItemPath(data, data.coverid, opf_folder, path_out)) {
    for (auto item : data.manifest) {
      if (item && item->id == data.coverid &&
          item->media_type.find("image/") == 0) {
        return true;
      }
    }
  }

  // 2) Heuristic: image item whose id/href/properties looks like "cover".
  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    if (ContainsNoCase(item->id, "cover") ||
        ContainsNoCase(item->href, "cover") ||
        ContainsNoCase(item->href, "portada") ||
        ContainsNoCase(item->properties, "cover")) {
      path_out = BuildDocPath(opf_folder, item->href);
      return true;
    }
  }

  // 3) Fallback: first image in manifest.
  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    path_out = BuildDocPath(opf_folder, item->href);
    return true;
  }

  path_out.clear();
  return false;
}

void epub_data_delete(epub_data_t *d);

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

int epub_parse_currentfile(unzFile uf, epub_data_t *epd) {
  int rc = 0;
  parsedata_t pd;
  char *filebuf = new char[BUFSIZE];
  XML_Parser p = XML_ParserCreate(NULL);
  if (epd->type == PARSE_CONTAINER) {
    XML_SetUserData(p, epd);
    XML_SetElementHandler(p, epub_container_start, NULL);
  } else if (epd->type == PARSE_ROOTFILE) {
    XML_SetUserData(p, epd);
    XML_SetElementHandler(p, epub_rootfile_start, epub_rootfile_end);
    XML_SetCharacterDataHandler(p, epub_rootfile_char);
  } else if (epd->type == PARSE_CONTENT) {
    epd->parsed_doc_title.clear();
    parse_init(&pd);
    pd.book = epd->book;
    pd.app = pd.book->GetApp();
    pd.ts = pd.app->ts;
    pd.prefs = pd.app->prefs;
    pd.docpath = epd->docpath;
    XML_SetUserData(p, &pd);
    XML_SetElementHandler(p, xml::book::start, xml::book::end);
    XML_SetCharacterDataHandler(p, xml::book::chardata);
    XML_SetDefaultHandler(p, xml::book::fallback);
    XML_SetProcessingInstructionHandler(p, xml::book::instruction);
  } else
    return 0;

  int len = 0;
  size_t len_total = 0;
  enum XML_Status status;
  do {
    len = unzReadCurrentFile(uf, filebuf, BUFSIZE);
    if (len < 0) {
      rc = len;
      break;
    }
    status = XML_Parse(p, filebuf, len, len == 0);
    if (status == XML_STATUS_ERROR) {
      rc = status;
      break;
    }
    len_total += (size_t)len;
  } while (len);

  XML_ParserFree(p);
  delete[] filebuf;
  if (epd->type == PARSE_CONTENT) {
    epd->parsed_doc_title = Trim(pd.doc_title);
    if (epd->parsed_doc_title.empty())
      epd->parsed_doc_title = Trim(pd.doc_heading);
  }
  return (rc);
}

static int LoadEpubPackageData(unzFile uf, Book *book, epub_data_t *parsedata,
                               std::string *opf_folder) {
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
  rc = epub_parse_currentfile(uf, parsedata);
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
  rc = epub_parse_currentfile(uf, parsedata);
  close_rc = unzCloseCurrentFile(uf);
  if (rc == 0 && close_rc != UNZ_OK)
    rc = close_rc;
  return rc;
}

static const epub_item *FindManifestItemById(const epub_data_t &data,
                                             const std::string &id) {
  for (auto item : data.manifest) {
    if (item && item->id == id)
      return item;
  }
  return nullptr;
}

static bool IsLikelyContentItem(const epub_item *item) {
  if (!item)
    return false;
  if (item->media_type == "application/xhtml+xml" ||
      item->media_type == "text/html" || item->media_type == "application/xml" ||
      item->media_type == "text/xml")
    return true;

  std::string href = item->href;
  std::transform(href.begin(), href.end(), href.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return (href.size() >= 6 && href.substr(href.size() - 6) == ".xhtml") ||
         (href.size() >= 5 && href.substr(href.size() - 5) == ".html") ||
         (href.size() >= 4 && href.substr(href.size() - 4) == ".htm");
}

static void BuildPageStartMapFromPackage(const epub_data_t &parsedata,
                                         const std::string &opf_folder,
                                         Book *book,
                                         std::map<std::string, u16> *out) {
  if (!book || !out)
    return;
  out->clear();

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

static bool LoadTocEntriesFromPackage(unzFile uf, epub_data_t &parsedata,
                                      const std::string &opf_folder,
                                      std::vector<toc_entry_t> *toc_entries,
                                      App *app) {
  if (!uf || !toc_entries)
    return false;

  toc_entries->clear();
  std::string toc_xml;
  std::string toc_doc_path;
  bool toc_loaded = false;

  auto looks_reasonable_toc = [](const std::vector<toc_entry_t> &entries) {
    if (entries.empty())
      return false;
    if (entries.size() == 1)
      return entries[0].title.size() <= 120;

    size_t too_long = 0;
    size_t empty_href = 0;
    std::set<std::string> unique_hrefs;
    for (const auto &e : entries) {
      if (e.title.size() > 140)
        too_long++;
      if (e.href.empty())
        empty_href++;
      else
        unique_hrefs.insert(StripFragmentAndQuery(e.href));
    }

    if (too_long * 4 > entries.size())  // >25% suspiciously long labels
      return false;
    if (empty_href * 2 > entries.size()) // >50% without href
      return false;
    if (entries.size() >= 6 && unique_hrefs.size() < 2)
      return false;
    return true;
  };

  if (!parsedata.navid.empty() &&
      FindManifestItemPath(parsedata, parsedata.navid, opf_folder, toc_doc_path)) {
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: try NAV %s", toc_doc_path.c_str());
      app->PrintStatus(msg);
    }
    if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NAV")) {
      std::vector<toc_entry_t> nav_entries;
      nav_parse_data_t navdata;
      navdata.entries = &nav_entries;
      navdata.base_path = toc_doc_path;
      navdata.depth = 0;
      navdata.toc_nav_depth = 0;
      navdata.anchor_depth = 0;
      if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata) &&
          looks_reasonable_toc(nav_entries)) {
        *toc_entries = nav_entries;
        toc_loaded = true;
      } else if (app) {
        app->PrintStatus("EPUB: NAV discarded (low-quality TOC)");
      }
    } else if (app) {
      app->PrintStatus("EPUB: NAV skipped (size/format)");
    }
  }

  if (!toc_loaded && !parsedata.tocid.empty() &&
      FindManifestItemPath(parsedata, parsedata.tocid, opf_folder, toc_doc_path)) {
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: try NCX %s", toc_doc_path.c_str());
      app->PrintStatus(msg);
    }
    if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NCX")) {
      std::vector<toc_entry_t> ncx_entries;
      if (ParseNcxLightweight(toc_xml, toc_doc_path, &ncx_entries, app) &&
          looks_reasonable_toc(ncx_entries)) {
        *toc_entries = ncx_entries;
        toc_loaded = true;
      } else if (app) {
        app->PrintStatus("EPUB: NCX discarded (low-quality TOC)");
      }
    } else if (app) {
      app->PrintStatus("EPUB: NCX skipped (size/format)");
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
          snprintf(msg, sizeof(msg), "EPUB: fallback NAV %s", toc_doc_path.c_str());
          app->PrintStatus(msg);
        }
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NAV-FALLBACK")) {
          std::vector<toc_entry_t> nav_entries;
          nav_parse_data_t navdata;
          navdata.entries = &nav_entries;
          navdata.base_path = toc_doc_path;
          navdata.depth = 0;
          navdata.toc_nav_depth = 0;
          navdata.anchor_depth = 0;
          if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata) &&
              looks_reasonable_toc(nav_entries)) {
            *toc_entries = nav_entries;
            toc_loaded = true;
          }
        } else if (app) {
          app->PrintStatus("EPUB: fallback NAV skipped");
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
          snprintf(msg, sizeof(msg), "EPUB: fallback NCX %s", toc_doc_path.c_str());
          app->PrintStatus(msg);
        }
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NCX-FALLBACK")) {
          std::vector<toc_entry_t> ncx_entries;
          if (ParseNcxLightweight(toc_xml, toc_doc_path, &ncx_entries, app) &&
              looks_reasonable_toc(ncx_entries)) {
            *toc_entries = ncx_entries;
            toc_loaded = true;
          }
        } else if (app) {
          app->PrintStatus("EPUB: fallback NCX skipped");
        }
        if (toc_loaded)
          break;
      }
    }
  }

  return !toc_entries->empty();
}

static std::string NormalizeTocTitle(const std::string &raw) {
  std::string t = Trim(raw);
  if (t.empty())
    return t;
  std::string out;
  out.reserve(t.size());
  bool prev_space = true;
  for (size_t i = 0; i < t.size(); i++) {
    unsigned char c = (unsigned char)t[i];
    bool is_space = isspace(c) || t[i] == '\n' || t[i] == '\r' || t[i] == '\t';
    if (is_space) {
      if (!prev_space)
        out.push_back(' ');
      prev_space = true;
    } else {
      out.push_back((char)c);
      prev_space = false;
    }
    if (out.size() >= 120)
      break;
  }
  return Trim(out);
}

int epub(Book *book, std::string name, bool metadataonly) {
  //! Parse EPUB file.
  //! Set metadataonly to true if you only want the title and author.
  int rc = 0;
  static epub_data_t parsedata;
  if (book && book->GetApp()) {
    book->GetApp()->PrintStatus("EPUB: parse begin");
  }

  // Parse top-level container XML for the rootfile.

  unzFile uf = unzOpen(name.c_str());
  if (!uf)
    return 1;
  rc = unzLocateFile(uf, "META-INF/container.xml", 2); // 2 = case insensitive
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 2;
  }
  rc = unzOpenCurrentFile(uf);
  epub_data_init(&parsedata);
  parsedata.book = book;
  parsedata.type = PARSE_CONTAINER;
  rc = epub_parse_currentfile(uf, &parsedata);
  rc = unzCloseCurrentFile(uf);

  // Extract any leading path for the rootfile.
  // The manifest in the rootfile will list filenames
  // relative to the rootfile location.
  std::string folder = "";
  size_t pos =
      parsedata.rootfile.find_last_of("/", parsedata.rootfile.length());
  if (pos < parsedata.rootfile.length()) {
    folder = parsedata.rootfile.substr(0, pos);
  }

  rc = unzLocateFile(uf, parsedata.rootfile.c_str(), 0);
  if (rc == UNZ_OK) {
    rc = unzOpenCurrentFile(uf);
    epub_data_init(&parsedata);
    parsedata.book = book;
    parsedata.type = PARSE_ROOTFILE;
    epub_parse_currentfile(uf, &parsedata);
    rc = unzCloseCurrentFile(uf);
  }

  // Stop here if only metadata is required.
  if (metadataonly) {
    if (parsedata.title.length()) {
      book->SetTitle(parsedata.title.c_str());
      if (parsedata.creator.length())
        book->SetAuthor(parsedata.creator);
    }
    // Find cover image path (explicit metadata first, then robust fallbacks).
    std::string coverpath;
    if (FindLikelyCoverImagePath(parsedata, folder, coverpath)) {
      book->coverImagePath = coverpath;
    } else {
      book->coverImagePath.clear();
    }
    unzClose(uf);
    epub_data_delete(&parsedata);
    if (book && book->GetApp()) {
      book->GetApp()->PrintStatus("EPUB: metadata done");
    }
    return rc;
  }

  // Read the XHTML in the manifest, ordering by spine if needed.
  parsedata.ctx.clear();
  parsedata.book = book;
  parsedata.type = PARSE_CONTENT;
  book->ClearChapters();
  book->ClearInlineImages();
  std::vector<std::string *> href;
  if (parsedata.spine.size()) {
    // Use spine for reading order.
    std::vector<epub_itemref *>::iterator itemref;
    for (itemref = parsedata.spine.begin(); itemref != parsedata.spine.end();
         itemref++) {
      std::vector<epub_item *>::iterator item;
      for (item = parsedata.manifest.begin(); item != parsedata.manifest.end();
           item++) {
        if ((*item)->id == (*itemref)->idref) {
          std::string *h = new std::string((*item)->href);
          href.push_back(h);
        }
      }
    }
  } else {
    std::vector<epub_item *>::iterator item;
    for (item = parsedata.manifest.begin(); item != parsedata.manifest.end();
         item++)
      href.push_back(new std::string((*item)->href));
  }

  std::map<std::string, u16> page_start_by_href;
  int chapter_num = 1;
  std::vector<std::string *>::iterator it;
  for (it = href.begin(); it != href.end(); it++) {
    size_t pos = (*it)->find_last_of('.');
    if (pos < (*it)->length()) {
      std::string path = BuildDocPath(folder, (*it)->c_str());
      std::string path_key = NormalizePath(path);

      rc = unzLocateFile(uf, path.c_str(), 2); // 2 = case insensitive
      if (rc == UNZ_OK) {
        u16 chapter_start_page = book->GetPageCount();
        std::string chapter_label = BuildChapterLabel(path, chapter_num);
        rc = unzOpenCurrentFile(uf);
        parsedata.docpath = path;
        epub_parse_currentfile(uf, &parsedata);
        rc = unzCloseCurrentFile(uf);
        std::string parsed_title =
            BuildChapterLabelFromText(parsedata.parsed_doc_title, chapter_num);
        if (!parsed_title.empty())
          chapter_label = parsed_title;
        chapter_num++;
        if (book->GetPageCount() > 0) {
          book->AddChapter(chapter_start_page, chapter_label);
          if (page_start_by_href.find(path_key) == page_start_by_href.end()) {
            page_start_by_href[path_key] = chapter_start_page;
          }
        }
      } else {
        char msg[256];
        sprintf(msg, "NOT FOUND IN ZIP: %s", path.c_str());
        book->GetApp()->PrintStatus(msg);
      }
    }
    delete *it;
  }
  if (book && book->GetApp()) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: content done pages=%u",
             (unsigned)book->GetPageCount());
    book->GetApp()->PrintStatus(msg);
  }

  // Replace fallback chapter names with real TOC/NAV names when available.
  if (EPUB_ENABLE_REAL_TOC_RESOLVE) {
    std::vector<toc_entry_t> toc_entries;
    std::string toc_xml;
    std::string toc_doc_path;
    bool toc_loaded = false;
    if (book && book->GetApp()) {
      book->GetApp()->PrintStatus("EPUB: TOC resolve begin");
    }

    if (!parsedata.navid.empty() &&
        FindManifestItemPath(parsedata, parsedata.navid, folder, toc_doc_path)) {
      if (book && book->GetApp()) {
        char msg[192];
        snprintf(msg, sizeof(msg), "EPUB: try NAV %s", toc_doc_path.c_str());
        book->GetApp()->PrintStatus(msg);
      }
      if (ReadZipEntryText(uf, toc_doc_path, toc_xml,
                           book ? book->GetApp() : NULL, "NAV")) {
        nav_parse_data_t navdata;
        navdata.entries = &toc_entries;
        navdata.base_path = toc_doc_path;
        navdata.depth = 0;
        navdata.toc_nav_depth = 0;
        navdata.anchor_depth = 0;
        if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata)) {
          toc_loaded = true;
        }
      } else if (book && book->GetApp()) {
        book->GetApp()->PrintStatus("EPUB: NAV skipped (size/format)");
      }
    }

    if (!toc_loaded && !parsedata.tocid.empty() &&
        FindManifestItemPath(parsedata, parsedata.tocid, folder, toc_doc_path)) {
      if (book && book->GetApp()) {
        char msg[192];
        snprintf(msg, sizeof(msg), "EPUB: try NCX %s", toc_doc_path.c_str());
        book->GetApp()->PrintStatus(msg);
      }
      if (ReadZipEntryText(uf, toc_doc_path, toc_xml,
                           book ? book->GetApp() : NULL, "NCX")) {
        if (ParseNcxLightweight(toc_xml, toc_doc_path, &toc_entries,
                                book ? book->GetApp() : NULL))
          toc_loaded = true;
      } else if (book && book->GetApp()) {
        book->GetApp()->PrintStatus("EPUB: NCX skipped (size/format)");
      }
    }

    if (!toc_loaded) {
      // Fallback discovery for malformed OPF metadata.
      for (auto item : parsedata.manifest) {
        if (item->media_type == "application/xhtml+xml" &&
            ContainsToken(item->properties, "nav")) {
          toc_doc_path = BuildDocPath(folder, item->href);
          if (book && book->GetApp()) {
            char msg[192];
            snprintf(msg, sizeof(msg), "EPUB: fallback NAV %s", toc_doc_path.c_str());
            book->GetApp()->PrintStatus(msg);
          }
          if (ReadZipEntryText(uf, toc_doc_path, toc_xml,
                               book ? book->GetApp() : NULL, "NAV-FALLBACK")) {
            nav_parse_data_t navdata;
            navdata.entries = &toc_entries;
            navdata.base_path = toc_doc_path;
            navdata.depth = 0;
            navdata.toc_nav_depth = 0;
            navdata.anchor_depth = 0;
            if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata))
              toc_loaded = true;
          } else if (book && book->GetApp()) {
            book->GetApp()->PrintStatus("EPUB: fallback NAV skipped");
          }
          if (toc_loaded)
            break;
        }
      }
    }

    if (!toc_loaded) {
      for (auto item : parsedata.manifest) {
        if (item->media_type == "application/x-dtbncx+xml") {
          toc_doc_path = BuildDocPath(folder, item->href);
          if (book && book->GetApp()) {
            char msg[192];
            snprintf(msg, sizeof(msg), "EPUB: fallback NCX %s", toc_doc_path.c_str());
            book->GetApp()->PrintStatus(msg);
          }
          if (ReadZipEntryText(uf, toc_doc_path, toc_xml, book ? book->GetApp() : NULL,
                               "NCX-FALLBACK")) {
            if (ParseNcxLightweight(toc_xml, toc_doc_path, &toc_entries,
                                    book ? book->GetApp() : NULL))
              toc_loaded = true;
          } else if (book && book->GetApp()) {
            book->GetApp()->PrintStatus("EPUB: fallback NCX skipped");
          }
          if (toc_loaded)
            break;
        }
      }
    }

    if (!toc_entries.empty()) {
      std::vector<ChapterEntry> resolved;
      std::map<u16, bool> used_pages;
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
        if (entry.title.empty())
          continue;
        resolved.push_back(entry);
      }

      if (!resolved.empty()) {
        book->ClearChapters();
        for (size_t i = 0; i < resolved.size(); i++) {
          book->AddChapter(resolved[i].page, resolved[i].title);
        }
      }
    }

    if (book && book->GetApp()) {
      char msg[96];
      snprintf(msg, sizeof(msg), "EPUB: toc entries=%u chapters=%u",
               (unsigned)toc_entries.size(), (unsigned)book->GetChapters().size());
      book->GetApp()->PrintStatus(msg);
      book->GetApp()->PrintStatus("EPUB: TOC resolve end");
    }
  } else if (book && book->GetApp()) {
    book->GetApp()->PrintStatus("EPUB: TOC resolve skipped (safe mode)");
  }

  unzClose(uf);
  epub_data_delete(&parsedata);
  if (book && book->GetApp()) {
    book->GetApp()->PrintStatus("EPUB: parse end");
  }
  return rc;
}

int epub_extract_cover(Book *book, const std::string &epubpath) {
  if (book->coverImagePath.empty())
    return 1;

  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return 2;

  int rc = unzLocateFile(uf, book->coverImagePath.c_str(), 2);
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 3;
  }

  unz_file_info fi;
  unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0);

  // Safety: limit uncompressed cover to 2 MB to avoid OOM on 3DS
  if (fi.uncompressed_size > 2 * 1024 * 1024) {
    unzClose(uf);
    return 8;
  }

  u8 *imgbuf = new u8[fi.uncompressed_size];
  unzOpenCurrentFile(uf);
  unzReadCurrentFile(uf, imgbuf, fi.uncompressed_size);
  unzCloseCurrentFile(uf);
  unzClose(uf);

  // Decode image using stb_image (supports JPEG + PNG)
  int imgW, imgH, channels;
  unsigned char *pixels = stbi_load_from_memory(
      imgbuf, fi.uncompressed_size, &imgW, &imgH, &channels, 3); // Force RGB
  delete[] imgbuf;

  if (!pixels)
    return 4; // Failed to decode image

  // Safety: skip images that are too large for 3DS RAM
  if (imgW > 2048 || imgH > 2048) {
    stbi_image_free(pixels);
    return 7;
  }

  // Scale to portrait thumbnail (85x115 to fit inside 89x119 button)
  int thumbW = 85;
  int thumbH = 115;
  float scaleX = (float)imgW / thumbW;
  float scaleY = (float)imgH / thumbH;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;
  int finalW = (int)(imgW / scale);
  int finalH = (int)(imgH / scale);
  if (finalW > thumbW)
    finalW = thumbW;
  if (finalH > thumbH)
    finalH = thumbH;

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
  book->coverPixels = new u16[finalW * finalH];
  book->coverWidth = finalW;
  book->coverHeight = finalH;

  for (int y = 0; y < finalH; y++) {
    int srcY = (int)(y * scale);
    if (srcY >= imgH)
      srcY = imgH - 1;
    for (int x = 0; x < finalW; x++) {
      int srcX = (int)(x * scale);
      if (srcX >= imgW)
        srcX = imgW - 1;
      unsigned char *px = &pixels[(srcY * imgW + srcX) * 3];
      // Convert RGB to RGB565
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * finalW + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return 0;
}

int epub_resolve_toc(Book *book, std::string filepath) {
  if (!book || filepath.empty())
    return 1;
  if (book->format != FORMAT_EPUB)
    return 2;
  if (book->GetChapters().empty())
    return 3;

  App *app = book->GetApp();
  if (app)
    app->PrintStatus("EPUB: TOC resolve begin");

  unzFile uf = unzOpen(filepath.c_str());
  if (!uf)
    return 4;

  epub_data_t parsedata;
  std::string opf_folder;
  int rc = LoadEpubPackageData(uf, book, &parsedata, &opf_folder);
  if (rc != 0) {
    epub_data_delete(&parsedata);
    unzClose(uf);
    return rc;
  }

  std::map<std::string, u16> page_start_by_href;
  BuildPageStartMapFromPackage(parsedata, opf_folder, book, &page_start_by_href);

  std::vector<toc_entry_t> toc_entries;
  bool loaded = LoadTocEntriesFromPackage(uf, parsedata, opf_folder, &toc_entries,
                                          app);
  if (!loaded) {
    if (app)
      app->PrintStatus("EPUB: TOC resolve end (no entries)");
    epub_data_delete(&parsedata);
    unzClose(uf);
    return 5;
  }

  std::vector<ChapterEntry> resolved;
  std::map<u16, bool> used_pages;
  size_t fallback_idx = 0;
  const std::vector<ChapterEntry> &current = book->GetChapters();
  const size_t kResolvedMaxEntries = 512;

  for (size_t i = 0; i < toc_entries.size(); i++) {
    std::string title = NormalizeTocTitle(toc_entries[i].title);
    if (title.empty())
      continue;

    u16 page = 0;
    bool have_page = false;

    const bool has_fragment = toc_entries[i].href.find('#') != std::string::npos;
    std::string key = NormalizePath(toc_entries[i].href);
    if (!key.empty()) {
      auto hit = page_start_by_href.find(key);
      if (hit == page_start_by_href.end()) {
        std::string key_no_fragment =
            NormalizePath(StripFragmentAndQuery(toc_entries[i].href));
        if (!key_no_fragment.empty())
          hit = page_start_by_href.find(key_no_fragment);
      }
      if (hit != page_start_by_href.end()) {
        page = hit->second;
        have_page = true;
      }
    }

    // If many TOC entries point to the same XHTML with different #anchors,
    // keep order by falling back to sequential chapter pages instead of
    // dropping duplicates on the same mapped page.
    if (have_page && used_pages[page] && has_fragment) {
      have_page = false;
    }

    if (!have_page) {
      while (fallback_idx < current.size() &&
             used_pages[current[fallback_idx].page]) {
        fallback_idx++;
      }
      if (fallback_idx < current.size()) {
        page = current[fallback_idx].page;
        have_page = true;
        fallback_idx++;
      }
    }

    if (!have_page || used_pages[page])
      continue;
    used_pages[page] = true;

    ChapterEntry entry;
    entry.page = page;
    entry.title = title;
    resolved.push_back(entry);
    if (resolved.size() >= kResolvedMaxEntries)
      break;
  }

  if (resolved.empty()) {
    if (app)
      app->PrintStatus("EPUB: TOC resolve end (unmatched)");
    epub_data_delete(&parsedata);
    unzClose(uf);
    return 6;
  }

  book->ClearChapters();
  for (size_t i = 0; i < resolved.size(); i++) {
    book->AddChapter(resolved[i].page, resolved[i].title);
  }

  if (app) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: toc entries=%u chapters=%u",
             (unsigned)toc_entries.size(), (unsigned)book->GetChapters().size());
    app->PrintStatus(msg);
    app->PrintStatus("EPUB: TOC resolve end");
  }

  epub_data_delete(&parsedata);
  unzClose(uf);
  return 0;
}
