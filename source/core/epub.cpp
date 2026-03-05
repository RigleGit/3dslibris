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
#include <stdio.h>
#include <string.h>
#include <vector>

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
  std::string ref = UrlDecode(StripFragmentAndQuery(ref_raw));
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
      toc_entry_t entry;
      entry.href = d->current_href;
      entry.title = title;
      d->entries->push_back(entry);
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

typedef struct {
  std::string src;
  std::string label;
} ncx_node_t;

typedef struct {
  std::vector<toc_entry_t> *entries;
  std::string base_path;
  int depth;
  int text_depth;
  std::string textbuf;
  std::vector<ncx_node_t> stack;
} ncx_parse_data_t;

static void ncx_start(void *userdata, const char *el, const char **attr) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  d->depth++;
  const char *lname = LocalName(el);

  if (!strcmp(lname, "navPoint")) {
    d->stack.push_back(ncx_node_t());
  } else if (!strcmp(lname, "content") && !d->stack.empty()) {
    const char *src = AttrValue(attr, "src");
    if (src && *src)
      d->stack.back().src = ResolveRelativePath(d->base_path, src);
  } else if (!strcmp(lname, "text") && !d->stack.empty()) {
    d->text_depth = d->depth;
    d->textbuf.clear();
  }
}

static void ncx_char(void *userdata, const XML_Char *txt, int len) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  if (d->text_depth > 0) {
    d->textbuf.append((const char *)txt, len);
  }
}

static void ncx_end(void *userdata, const char *el) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  const char *lname = LocalName(el);

  if (d->text_depth == d->depth && !strcmp(lname, "text")) {
    if (!d->stack.empty()) {
      std::string label = Trim(d->textbuf);
      if (!label.empty() && d->stack.back().label.empty())
        d->stack.back().label = label;
    }
    d->text_depth = 0;
    d->textbuf.clear();
  } else if (!strcmp(lname, "navPoint") && !d->stack.empty()) {
    ncx_node_t node = d->stack.back();
    d->stack.pop_back();
    if (!node.src.empty() && !node.label.empty()) {
      toc_entry_t entry;
      entry.href = node.src;
      entry.title = node.label;
      d->entries->push_back(entry);
    }
  }

  d->depth--;
}

static bool ParseXmlBuffer(const std::string &xml, XML_StartElementHandler start,
                           XML_EndElementHandler end,
                           XML_CharacterDataHandler chardata, void *userdata) {
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

static bool ReadZipEntryText(unzFile uf, const std::string &path, std::string &out) {
  out.clear();
  if (path.empty())
    return false;
  if (unzLocateFile(uf, path.c_str(), 2) != UNZ_OK)
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  char buf[BUFSIZE];
  int n = 0;
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    out.append(buf, n);
  }
  unzCloseCurrentFile(uf);
  return n >= 0;
}

static std::string BuildDocPath(const std::string &opf_folder,
                                const std::string &href) {
  if (opf_folder.empty())
    return NormalizePath(UrlDecode(href));
  return NormalizePath(opf_folder + "/" + UrlDecode(href));
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

void epub_data_delete(epub_data_t *d);

void epub_data_init(epub_data_t *d) {
  // Reset any leftover heap objects from previous parses.
  epub_data_delete(d);

  d->type = PARSE_CONTAINER;
  d->ctx.push_back(new std::string("TOP"));
  d->rootfile = "";
  d->title = "";
  d->creator = "";
  d->coverid = "";
  d->tocid = "";
  d->navid = "";
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
    parse_init(&pd);
    pd.book = epd->book;
    pd.app = pd.book->GetApp();
    pd.ts = pd.app->ts;
    pd.prefs = pd.app->prefs;
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
  return (rc);
}

int epub(Book *book, std::string name, bool metadataonly) {
  //! Parse EPUB file.
  //! Set metadataonly to true if you only want the title and author.
  int rc = 0;
  static epub_data_t parsedata;

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
    // Find cover image path from manifest
    if (parsedata.coverid.length()) {
      for (auto item : parsedata.manifest) {
        if (item->id == parsedata.coverid) {
          std::string coverpath = folder;
          if (coverpath.length())
            coverpath += "/";
          coverpath += item->href;
          book->coverImagePath = coverpath;
          break;
        }
      }
    }
    unzClose(uf);
    epub_data_delete(&parsedata);
    return rc;
  }

  // Read the XHTML in the manifest, ordering by spine if needed.
  parsedata.ctx.clear();
  parsedata.book = book;
  parsedata.type = PARSE_CONTENT;
  book->ClearChapters();
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
        std::string chapter_label = BuildChapterLabel(path, chapter_num++);
        rc = unzOpenCurrentFile(uf);
        epub_parse_currentfile(uf, &parsedata);
        rc = unzCloseCurrentFile(uf);
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

  // Replace fallback chapter names with real TOC/NAV names when available.
  std::vector<toc_entry_t> toc_entries;
  std::string toc_xml;
  std::string toc_doc_path;
  bool toc_loaded = false;

  if (!parsedata.navid.empty() &&
      FindManifestItemPath(parsedata, parsedata.navid, folder, toc_doc_path)) {
    if (ReadZipEntryText(uf, toc_doc_path, toc_xml)) {
      nav_parse_data_t navdata;
      navdata.entries = &toc_entries;
      navdata.base_path = toc_doc_path;
      navdata.depth = 0;
      navdata.toc_nav_depth = 0;
      navdata.anchor_depth = 0;
      if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata)) {
        toc_loaded = true;
      }
    }
  }

  if (!toc_loaded && !parsedata.tocid.empty() &&
      FindManifestItemPath(parsedata, parsedata.tocid, folder, toc_doc_path)) {
    if (ReadZipEntryText(uf, toc_doc_path, toc_xml)) {
      ncx_parse_data_t ncxdata;
      ncxdata.entries = &toc_entries;
      ncxdata.base_path = toc_doc_path;
      ncxdata.depth = 0;
      ncxdata.text_depth = 0;
      if (ParseXmlBuffer(toc_xml, ncx_start, ncx_end, ncx_char, &ncxdata)) {
        toc_loaded = true;
      }
    }
  }

  if (!toc_loaded) {
    // Fallback discovery for malformed OPF metadata.
    for (auto item : parsedata.manifest) {
      if (item->media_type == "application/xhtml+xml" &&
          ContainsToken(item->properties, "nav")) {
        toc_doc_path = BuildDocPath(folder, item->href);
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml)) {
          nav_parse_data_t navdata;
          navdata.entries = &toc_entries;
          navdata.base_path = toc_doc_path;
          navdata.depth = 0;
          navdata.toc_nav_depth = 0;
          navdata.anchor_depth = 0;
          if (ParseXmlBuffer(toc_xml, nav_start, nav_end, nav_char, &navdata))
            toc_loaded = true;
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
        if (ReadZipEntryText(uf, toc_doc_path, toc_xml)) {
          ncx_parse_data_t ncxdata;
          ncxdata.entries = &toc_entries;
          ncxdata.base_path = toc_doc_path;
          ncxdata.depth = 0;
          ncxdata.text_depth = 0;
          if (ParseXmlBuffer(toc_xml, ncx_start, ncx_end, ncx_char, &ncxdata))
            toc_loaded = true;
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

  unzClose(uf);
  epub_data_delete(&parsedata);
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
