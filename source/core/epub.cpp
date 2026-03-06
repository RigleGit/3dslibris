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
static bool ParseXmlBuffer(const std::string &xml, XML_StartElementHandler start,
                           XML_EndElementHandler end,
                           XML_CharacterDataHandler chardata, void *userdata);

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

static std::string ToLowerAscii(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static std::string BasenamePath(const std::string &path) {
  if (path.empty())
    return "";
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return "";
  return path.substr(slash + 1);
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

typedef struct {
  int depth;
  std::string src;
  std::string title;
  bool in_text;
  int text_depth;
} ncx_navpoint_state_t;

typedef struct {
  std::vector<toc_entry_t> *entries;
  std::string base_path;
  int depth;
  std::vector<ncx_navpoint_state_t> stack;
} ncx_parse_data_t;

typedef struct {
  std::map<std::string, std::string> *fragment_to_href;
  std::string base_path;
  std::vector<std::string> id_stack;
  std::vector<bool> pushed_stack;
} toc_proxy_parse_data_t;

static void EpubDiag(App *app, const char *fmt, const char *arg = NULL);

static std::string NormalizeFragmentId(const std::string &raw) {
  std::string frag = Trim(UrlDecode(raw));
  while (!frag.empty() && frag[0] == '#')
    frag.erase(frag.begin());
  size_t q = frag.find('?');
  if (q != std::string::npos)
    frag = frag.substr(0, q);
  return frag;
}

static std::string ExtractHrefFragment(const std::string &href) {
  size_t hash = href.find('#');
  if (hash == std::string::npos || hash + 1 >= href.size())
    return "";
  return NormalizeFragmentId(href.substr(hash + 1));
}

static void toc_proxy_start(void *userdata, const char *el, const char **attr) {
  toc_proxy_parse_data_t *d = (toc_proxy_parse_data_t *)userdata;
  bool pushed = false;

  std::string current_id;
  const char *id = AttrValue(attr, "id");
  if (!id || !*id)
    id = AttrValue(attr, "name");
  if (id && *id) {
    current_id = NormalizeFragmentId(id);
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
      if (!current_id.empty()) {
        if (d->fragment_to_href->find(current_id) == d->fragment_to_href->end())
          (*d->fragment_to_href)[current_id] = resolved;
      } else if (!d->id_stack.empty()) {
        const std::string &id_from_parent = d->id_stack.back();
        if (d->fragment_to_href->find(id_from_parent) ==
            d->fragment_to_href->end()) {
          (*d->fragment_to_href)[id_from_parent] = resolved;
        }
      }
    }
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

static void ncx_start(void *userdata, const char *el, const char **attr) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  d->depth++;
  const char *lname = LocalName(el);

  if (!strcmp(lname, "navPoint")) {
    ncx_navpoint_state_t node;
    node.depth = d->depth;
    node.src.clear();
    node.title.clear();
    node.in_text = false;
    node.text_depth = 0;
    d->stack.push_back(node);
    return;
  }

  if (d->stack.empty())
    return;

  ncx_navpoint_state_t &node = d->stack.back();
  if (!strcmp(lname, "content")) {
    const char *src = AttrValue(attr, "src");
    if (src && *src && node.src.empty())
      node.src = src;
  } else if (!strcmp(lname, "text")) {
    node.in_text = true;
    node.text_depth = d->depth;
  }
}

static void ncx_char(void *userdata, const XML_Char *txt, int len) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  if (d->stack.empty())
    return;
  ncx_navpoint_state_t &node = d->stack.back();
  if (node.in_text)
    node.title.append((const char *)txt, len);
}

static void ncx_end(void *userdata, const char *el) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  const char *lname = LocalName(el);

  if (!d->stack.empty()) {
    ncx_navpoint_state_t &node = d->stack.back();
    if (!strcmp(lname, "text") && node.in_text && node.text_depth == d->depth) {
      node.in_text = false;
      node.text_depth = 0;
    }
  }

  if (!d->stack.empty() && !strcmp(lname, "navPoint") &&
      d->stack.back().depth == d->depth) {
    ncx_navpoint_state_t node = d->stack.back();
    d->stack.pop_back();

    std::string title = Trim(node.title);
    if (!node.src.empty() && !title.empty() &&
        d->entries->size() < EPUB_TOC_MAX_ENTRIES) {
      toc_entry_t entry;
      entry.href = ResolveRelativePath(d->base_path, node.src);
      entry.title = title;
      if (!entry.href.empty() && !entry.title.empty())
        d->entries->push_back(entry);
    }
  }

  d->depth--;
}

static bool ParseNcxWithExpat(const std::string &xml,
                              const std::string &base_path,
                              std::vector<toc_entry_t> *entries, App *app) {
  if (xml.empty() || !entries)
    return false;
  if (xml.size() > EPUB_TOC_MAX_BYTES)
    return false;

  if (app)
    app->PrintStatus("EPUB: NCX expat parse begin");

  ncx_parse_data_t d;
  d.entries = entries;
  d.base_path = base_path;
  d.depth = 0;
  d.stack.clear();
  bool ok = ParseXmlBuffer(xml, ncx_start, ncx_end, ncx_char, &d);

  if (app) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: NCX expat parse end ok=%u entries=%u",
             ok ? 1u : 0u, (unsigned)entries->size());
    app->PrintStatus(msg);
  }

  return ok && !entries->empty();
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

static std::string NormalizeZipEntryName(const std::string &name) {
  std::string n = name;
  std::replace(n.begin(), n.end(), '\\', '/');
  return n;
}

static bool EqualsAsciiNoCase(const std::string &a, const std::string &b) {
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
}

static bool LocateZipEntrySafe(unzFile uf, const std::string &entry_path,
                               App *app, const char *tag) {
  if (!uf || entry_path.empty())
    return false;

  const char *t = (tag && *tag) ? tag : "ZIP";

  // Fast path: exact match first.
  if (unzLocateFile(uf, entry_path.c_str(), 0) == UNZ_OK)
    return true;

  // Fallback: iterate entries and compare with ASCII-only case folding.
  int rc = unzGoToFirstFile(uf);
  if (rc != UNZ_OK) {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: %s locate first fail rc=%d", t, rc);
    if (app)
      app->PrintStatus(msg);
    return false;
  }

  std::string wanted = NormalizeZipEntryName(entry_path);
  size_t scanned = 0;
  do {
    scanned++;
    unz_file_info fi;
    char fname[1024];
    int info_rc =
        unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL, 0);
    if (info_rc == UNZ_OK) {
      std::string current = NormalizeZipEntryName(std::string(fname));
      if (current == wanted || EqualsAsciiNoCase(current, wanted)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "EPUB: %s locate fallback hit scanned=%u", t,
                 (unsigned)scanned);
        if (app)
          app->PrintStatus(msg);
        return true;
      }
    }

    if (scanned > 32768) {
      char msg[160];
      snprintf(msg, sizeof(msg), "EPUB: %s locate fallback abort scanned=%u", t,
               (unsigned)scanned);
      if (app)
        app->PrintStatus(msg);
      break;
    }
    rc = unzGoToNextFile(uf);
  } while (rc == UNZ_OK);

  {
    char msg[160];
    snprintf(msg, sizeof(msg), "EPUB: %s locate fallback miss scanned=%u rc=%d",
             t, (unsigned)scanned, rc);
    if (app)
      app->PrintStatus(msg);
  }
  return false;
}

static bool ReadZipEntryText(unzFile uf, const std::string &path, std::string &out,
                             App *app = NULL, const char *tag = NULL) {
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
  if (!LocateZipEntrySafe(uf, path, app, t)) {
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

  // 3DS stack is tight; avoid large BUFSIZE (128KB) on stack here.
  char buf[8 * 1024];
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

static bool
BuildTocFragmentProxyMap(unzFile uf, const std::string &doc_path,
                         std::map<std::string, std::string> *proxy_map,
                         App *app) {
  if (!proxy_map || doc_path.empty())
    return false;
  proxy_map->clear();

  std::string xml;
  if (!ReadZipEntryText(uf, doc_path, xml, app, "TOC-PROXY"))
    return false;

  toc_proxy_parse_data_t data;
  data.fragment_to_href = proxy_map;
  data.base_path = doc_path;
  data.id_stack.clear();
  data.pushed_stack.clear();

  if (!ParseXmlBuffer(xml, toc_proxy_start, toc_proxy_end, NULL, &data))
    return false;

  return !proxy_map->empty();
}

static bool LookupTocProxyHref(
    unzFile uf, const std::string &doc_path, const std::string &fragment_raw,
    std::map<std::string, std::map<std::string, std::string>> *cache,
    std::set<std::string> *attempted, std::string *href_out, App *app) {
  if (!cache || !attempted || !href_out || doc_path.empty())
    return false;
  href_out->clear();

  std::string fragment = NormalizeFragmentId(fragment_raw);
  if (fragment.empty())
    return false;

  if (attempted->find(doc_path) == attempted->end()) {
    std::map<std::string, std::string> local_map;
    BuildTocFragmentProxyMap(uf, doc_path, &local_map, app);
    (*cache)[doc_path] = local_map;
    attempted->insert(doc_path);
  }

  auto hit_doc = cache->find(doc_path);
  if (hit_doc == cache->end())
    return false;
  auto hit = hit_doc->second.find(fragment);
  if (hit == hit_doc->second.end())
    return false;

  *href_out = hit->second;
  return !href_out->empty();
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

  const auto &cached = book->GetChapterDocStartPages();
  if (!cached.empty()) {
    for (const auto &kv : cached) {
      (*out)[kv.first] = kv.second;
    }
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

    if (too_long * 4 > entries.size())  // >25% suspiciously long labels
      return false;
    if (empty_href * 2 > entries.size()) // >50% without href
      return false;
    // Allow single-document TOCs (common in old EPUBs) when entries are mostly
    // in-document anchors (chapter.xhtml#id).
    if (entries.size() >= 6 && unique_hrefs.size() < 2 &&
        with_fragment * 3 < entries.size() * 2)
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
    if (app)
      app->PrintStatus("EPUB: NCX read call begin");
    bool ncx_read_ok = ReadZipEntryText(uf, toc_doc_path, toc_xml, app, "NCX");
    if (app) {
      app->PrintStatus(ncx_read_ok ? "EPUB: NCX read call ok"
                                   : "EPUB: NCX read call fail");
    }
    if (ncx_read_ok) {
      std::vector<toc_entry_t> ncx_entries;
      bool parsed = ParseNcxWithExpat(toc_xml, toc_doc_path, &ncx_entries, app);
      if (!parsed)
        parsed = ParseNcxLightweight(toc_xml, toc_doc_path, &ncx_entries, app);
      if (parsed && looks_reasonable_toc(ncx_entries)) {
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
          bool parsed =
              ParseNcxWithExpat(toc_xml, toc_doc_path, &ncx_entries, app);
          if (!parsed)
            parsed =
                ParseNcxLightweight(toc_xml, toc_doc_path, &ncx_entries, app);
          if (parsed && looks_reasonable_toc(ncx_entries)) {
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

static std::string ClipForDiag(const std::string &s, size_t max_bytes = 120) {
  if (s.size() <= max_bytes)
    return s;
  return s.substr(0, max_bytes) + "...";
}

static std::string NormalizeAsciiSearchText(const std::string &raw,
                                            size_t max_out = 0) {
  if (raw.empty())
    return "";
  std::string out;
  out.reserve(raw.size());
  bool prev_space = true;
  for (size_t i = 0; i < raw.size(); i++) {
    unsigned char c = (unsigned char)raw[i];
    if (c < 0x80) {
      if (isalnum(c)) {
        out.push_back((char)tolower(c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
    } else {
      // Keep UTF-8 bytes to preserve accented text for exact byte matching.
      out.push_back((char)c);
      prev_space = false;
    }
    if (max_out > 0 && out.size() >= max_out)
      break;
  }
  return Trim(out);
}

static std::string BuildPageSearchText(Page *page, size_t max_out = 2048) {
  if (!page || !page->GetBuffer() || page->GetLength() <= 0)
    return "";
  const u8 *buf = page->GetBuffer();
  const int len = page->GetLength();

  std::string out;
  out.reserve((size_t)len);
  bool prev_space = true;

  int i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)buf[i];
    if (c == TEXT_IMAGE) {
      i += (i + 2 < len) ? 3 : 1;
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF) {
      i++;
      continue;
    }
    if (c < 0x20) {
      i++;
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c < 0x80) {
      i++;
      if (isalnum(c)) {
        out.push_back((char)tolower(c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      if (max_out > 0 && out.size() >= max_out)
        break;
      continue;
    }

    int step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;
    if (i + step > len)
      step = 1;
    for (int j = 1; j < step; j++) {
      if (((unsigned char)buf[i + j] & 0xC0) != 0x80) {
        step = 1;
        break;
      }
    }
    for (int j = 0; j < step; j++)
      out.push_back((char)buf[i + j]);
    prev_space = false;
    i += step;
    if (max_out > 0 && out.size() >= max_out)
      break;
  }

  return Trim(out);
}

static bool PathLooksLikeTocDocForFallback(const std::string &path) {
  if (path.empty())
    return false;
  std::string lower = ToLowerAscii(path);
  return lower.find("toc") != std::string::npos ||
         lower.find("indice") != std::string::npos ||
         lower.find("index") != std::string::npos ||
         lower.find("contents") != std::string::npos ||
         lower.find("contenido") != std::string::npos ||
         lower.find("nav") != std::string::npos;
}

static std::vector<std::string>
ExtractTitleSearchTokens(const std::string &normalized_title) {
  std::vector<std::string> tokens;
  std::string cur;
  for (size_t i = 0; i <= normalized_title.size(); i++) {
    unsigned char c =
        (i < normalized_title.size()) ? (unsigned char)normalized_title[i] : 0;
    if (c >= 'a' && c <= 'z') {
      cur.push_back((char)c);
    } else if (c >= '0' && c <= '9') {
      cur.push_back((char)c);
    } else {
      if (cur.size() >= 4)
        tokens.push_back(cur);
      cur.clear();
    }
  }
  if (tokens.empty())
    return tokens;
  std::sort(tokens.begin(), tokens.end(),
            [](const std::string &a, const std::string &b) {
              if (a.size() != b.size())
                return a.size() > b.size();
              return a < b;
            });
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  if (tokens.size() > 8)
    tokens.resize(8);
  return tokens;
}

static u16 FindDocEndPage(u16 doc_start, const std::vector<u16> &doc_starts,
                          u16 page_count) {
  for (size_t i = 0; i < doc_starts.size(); i++) {
    if (doc_starts[i] != doc_start)
      continue;
    if (i + 1 < doc_starts.size())
      return doc_starts[i + 1];
    return page_count;
  }
  for (size_t i = 0; i < doc_starts.size(); i++) {
    if (doc_starts[i] > doc_start)
      return doc_starts[i];
  }
  return page_count;
}

static bool FindTocTitlePageInDocRange(Book *book, u16 doc_start,
                                       const std::vector<u16> &doc_starts,
                                       const std::string &toc_title,
                                       u16 *page_out) {
  if (!book || !page_out)
    return false;

  const u16 page_count = book->GetPageCount();
  if (doc_start >= page_count)
    return false;

  const u16 doc_end = FindDocEndPage(doc_start, doc_starts, page_count);
  if (doc_end <= doc_start + 1)
    return false;

  std::string query = NormalizeAsciiSearchText(toc_title, 192);
  if (query.empty())
    return false;
  std::vector<std::string> tokens = ExtractTitleSearchTokens(query);
  if (tokens.empty() && query.size() < 8)
    return false;

  int best_score = -1;
  int best_hits = 0;
  u16 best_page = doc_start;

  for (u16 p = doc_start; p < doc_end && p < page_count; p++) {
    Page *page = book->GetPage((int)p);
    std::string text = BuildPageSearchText(page, 4096);
    if (text.empty())
      continue;
    if (query.size() >= 10 && text.find(query) != std::string::npos) {
      *page_out = p;
      return true;
    }

    int score = 0;
    int hits = 0;
    for (size_t i = 0; i < tokens.size() && i < 6; i++) {
      if (text.find(tokens[i]) != std::string::npos) {
        score += (int)tokens[i].size();
        hits++;
      }
    }
    if (score > best_score) {
      best_score = score;
      best_hits = hits;
      best_page = p;
    }
  }

  if (best_score >= 12 || (best_score >= 8 && best_hits >= 2)) {
    *page_out = best_page;
    return true;
  }
  return false;
}

static bool FindTocTitlePageGlobal(Book *book, const std::string &toc_title,
                                   u16 from_page, u16 *page_out,
                                   bool allow_wrap = true) {
  if (!book || !page_out)
    return false;
  u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;
  if (from_page >= page_count)
    from_page = 0;

  std::string query = NormalizeAsciiSearchText(toc_title, 192);
  if (query.empty())
    return false;
  std::vector<std::string> tokens = ExtractTitleSearchTokens(query);
  if (tokens.empty() && query.size() < 8)
    return false;

  int best_score = -1;
  int best_hits = 0;
  u16 best_page = from_page;

  for (u16 pass = 0; pass < (allow_wrap ? 2 : 1); pass++) {
    u16 p0 = (pass == 0) ? from_page : 0;
    u16 p1 = (pass == 0) ? page_count : from_page;
    for (u16 p = p0; p < p1; p++) {
      Page *page = book->GetPage((int)p);
      std::string text = BuildPageSearchText(page, 4096);
      if (text.empty())
        continue;

      if (query.size() >= 10 && text.find(query) != std::string::npos) {
        *page_out = p;
        return true;
      }

      int score = 0;
      int hits = 0;
      for (size_t i = 0; i < tokens.size() && i < 6; i++) {
        if (text.find(tokens[i]) != std::string::npos) {
          score += (int)tokens[i].size();
          hits++;
        }
      }
      if (score > best_score) {
        best_score = score;
        best_hits = hits;
        best_page = p;
      }
    }
  }

  if (best_score >= 12 || (best_score >= 8 && best_hits >= 2)) {
    *page_out = best_page;
    return true;
  }
  return false;
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
  book->ClearChapterDocStartPages();
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
          book->SetChapterDocStartPage(path_key, chapter_start_page);
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
        bool parsed = ParseNcxWithExpat(toc_xml, toc_doc_path, &toc_entries,
                                        book ? book->GetApp() : NULL);
        if (!parsed)
          parsed = ParseNcxLightweight(toc_xml, toc_doc_path, &toc_entries,
                                       book ? book->GetApp() : NULL);
        if (parsed)
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
            bool parsed =
                ParseNcxWithExpat(toc_xml, toc_doc_path, &toc_entries,
                                  book ? book->GetApp() : NULL);
            if (!parsed)
              parsed = ParseNcxLightweight(toc_xml, toc_doc_path, &toc_entries,
                                           book ? book->GetApp() : NULL);
            if (parsed)
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
  if (app)
    app->PrintStatus("EPUB: TOC open zip begin");

  unzFile uf = unzOpen(filepath.c_str());
  if (!uf)
    return 4;
  if (app)
    app->PrintStatus("EPUB: TOC open zip ok");

  epub_data_t parsedata;
  std::string opf_folder;
  if (app)
    app->PrintStatus("EPUB: TOC package load begin");
  int rc = LoadEpubPackageData(uf, book, &parsedata, &opf_folder);
  if (rc != 0) {
    if (app) {
      char msg[96];
      snprintf(msg, sizeof(msg), "EPUB: TOC package load fail rc=%d", rc);
      app->PrintStatus(msg);
    }
    epub_data_delete(&parsedata);
    unzClose(uf);
    return rc;
  }
  if (app)
    app->PrintStatus("EPUB: TOC package load ok");

  std::map<std::string, u16> page_start_by_href;
  if (app)
    app->PrintStatus("EPUB: TOC map build begin");
  BuildPageStartMapFromPackage(parsedata, opf_folder, book, &page_start_by_href);
  if (app) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: TOC map build ok size=%u",
             (unsigned)page_start_by_href.size());
    app->PrintStatus(msg);
  }

  std::vector<toc_entry_t> toc_entries;
  if (app)
    app->PrintStatus("EPUB: TOC entries load begin");
  bool loaded = LoadTocEntriesFromPackage(uf, parsedata, opf_folder, &toc_entries,
                                          app);
  if (app) {
    char msg[96];
    snprintf(msg, sizeof(msg), "EPUB: TOC entries load end loaded=%u count=%u",
             loaded ? 1u : 0u, (unsigned)toc_entries.size());
    app->PrintStatus(msg);
  }
  if (!loaded) {
    if (app)
      app->PrintStatus("EPUB: TOC resolve end (no entries)");
    epub_data_delete(&parsedata);
    unzClose(uf);
    return 5;
  }

  std::vector<ChapterEntry> resolved;
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
  size_t stat_proxy = 0;
  size_t stat_with_fragment = 0;
  size_t stat_lc = 0;
  size_t stat_base = 0;
  size_t stat_skip_unmatched = 0;
  size_t stat_skip_dup = 0;
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
  const u16 total_pages = book->GetPageCount();
  bool have_last_resolved_page = false;
  u16 last_resolved_page = 0;

  for (size_t i = 0; i < toc_entries.size(); i++) {
    std::string title = NormalizeTocTitle(toc_entries[i].title);
    if (title.empty())
      continue;

    u16 page = 0;
    bool have_page = false;
    bool page_from_doc_start = false;
    bool anchor_lookup_failed = false;
    std::string mapped_doc_key;

    const bool has_fragment = toc_entries[i].href.find('#') != std::string::npos;
    if (has_fragment)
      stat_with_fragment++;
    if (has_fragment) {
      u16 anchor_page = 0;
      if (book->FindChapterAnchorPage(toc_entries[i].href, &anchor_page)) {
        page = anchor_page;
        have_page = true;
        stat_anchor++;
      } else {
        stat_anchor_miss++;
        anchor_lookup_failed = true;
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
        }
      }
      if (!have_page) {
        std::string key_no_fragment_lc =
            ToLowerAscii(NormalizePath(StripFragmentAndQuery(toc_entries[i].href)));
        auto hit_nofrag_lc = page_start_by_href_lc.find(key_no_fragment_lc);
        if (hit_nofrag_lc != page_start_by_href_lc.end()) {
          page = hit_nofrag_lc->second;
          have_page = true;
          stat_lc++;
          page_from_doc_start = true;
          mapped_doc_key = key_no_fragment_lc;
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
          }
        }
      }
    }

    bool mapped_is_toc_doc = PathLooksLikeTocDocForFallback(mapped_doc_key);
    if (has_fragment && anchor_lookup_failed && have_page && page_from_doc_start &&
        mapped_is_toc_doc) {
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
            std::string proxy_nofrag = NormalizePath(StripFragmentAndQuery(proxy_href));
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
        }
      }
    }

    if (has_fragment && anchor_lookup_failed && have_page && page_from_doc_start) {
      u16 title_page = 0;
      bool got_title = false;
      if (!mapped_is_toc_doc) {
        got_title =
            FindTocTitlePageInDocRange(book, page, doc_starts, title, &title_page);
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
        got_title =
            FindTocTitlePageGlobal(book, title, from_page, &title_page, false);
        if (got_title)
          stat_title_global++;
      }
      if (got_title) {
        page = title_page;
        stat_title_fallback++;
      } else if (unresolved_fragment_samples.size() < 3) {
        unresolved_fragment_samples.push_back(toc_entries[i].href);
      }

      // If this came from a TOC/index document and we still couldn't resolve
      // by title, avoid emitting a broken chapter entry (often page 0).
      if (!got_title && mapped_is_toc_doc) {
        have_page = false;
      }
    }

    if (!have_page) {
      stat_skip_unmatched++;
      continue;
    }
    if (!has_fragment && used_pages_non_fragment[page]) {
      stat_skip_dup++;
      continue;
    }
    if (!has_fragment)
      used_pages_non_fragment[page] = true;

    ChapterEntry entry;
    entry.page = page;
    entry.title = title;
    resolved.push_back(entry);
    have_last_resolved_page = true;
    last_resolved_page = page;
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
    char map_msg[192];
    snprintf(map_msg, sizeof(map_msg),
             "EPUB: TOC map stats anchor=%u exact=%u nofrag=%u lower=%u base=%u proxy=%u titlefb=%u titleg=%u skip=%u dup=%u",
             (unsigned)stat_anchor, (unsigned)stat_exact, (unsigned)stat_nofrag,
             (unsigned)stat_lc, (unsigned)stat_base, (unsigned)stat_proxy,
             (unsigned)stat_title_fallback, (unsigned)stat_title_global,
             (unsigned)stat_skip_unmatched, (unsigned)stat_skip_dup);
    app->PrintStatus(map_msg);
    if (stat_anchor_miss > 0) {
      char warn_msg[160];
      snprintf(warn_msg, sizeof(warn_msg),
               "EPUB: TOC fragments unresolved=%u anchor_map=%u titlefb=%u",
               (unsigned)stat_anchor_miss,
               (unsigned)book->GetChapterAnchorCount(),
               (unsigned)stat_title_fallback);
      app->PrintStatus(warn_msg);
      for (size_t i = 0; i < unresolved_fragment_samples.size(); i++) {
        char sample_msg[192];
        std::string clipped = ClipForDiag(unresolved_fragment_samples[i], 100);
        snprintf(sample_msg, sizeof(sample_msg), "EPUB: TOC unresolved href[%u]=%s",
                 (unsigned)i, clipped.c_str());
        app->PrintStatus(sample_msg);
      }
    } else if (stat_with_fragment > 0 && stat_anchor == 0) {
      char warn_msg[160];
      snprintf(warn_msg, sizeof(warn_msg),
               "EPUB: TOC fragments unresolved=%u anchor_map=%u titlefb=%u",
               (unsigned)stat_with_fragment,
               (unsigned)book->GetChapterAnchorCount(),
               (unsigned)stat_title_fallback);
      app->PrintStatus(warn_msg);
    }
    app->PrintStatus("EPUB: TOC resolve end");
  }

  epub_data_delete(&parsedata);
  unzClose(uf);
  return 0;
}
