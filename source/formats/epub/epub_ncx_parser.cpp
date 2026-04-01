/*
    3dslibris - epub_ncx_parser.cpp
    NCX and EPUB3 NAV document parsing.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_ncx_parser.h"

#include "formats/common/xml_parse_utils.h"
#include <algorithm>
#include <ctype.h>
#include <set>
#include <string.h>

namespace {

static const size_t EPUB_TOC_MAX_BYTES = 192 * 1024;
static const size_t EPUB_TOC_MAX_ENTRIES = 2048;

static const char *LocalName(const char *name) {
  const char *colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

static const char *AttrValue(const char **attr, const char *key) {
  for (int i = 0; attr[i]; i += 2) {
    if (!strcmp(LocalName(attr[i]), key))
      return attr[i + 1];
  }
  return NULL;
}

static std::string Trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

static std::string ResolveRelativePath(const std::string &base,
                                       const std::string &rel) {
  if (rel.empty())
    return rel;
  if (rel.find("://") != std::string::npos || rel[0] == '/')
    return rel;

  std::string dir = base;
  size_t slash = dir.find_last_of('/');
  if (slash != std::string::npos)
    dir = dir.substr(0, slash + 1);
  else
    dir = "";

  std::string result = dir + rel;

  size_t pos = 0;
  while ((pos = result.find("./", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  while (true) {
    size_t pos = result.find("/../");
    if (pos == std::string::npos)
      break;
    size_t prev = result.rfind('/', pos - 1);
    if (prev == std::string::npos) {
      result.erase(0, pos + 4);
    } else {
      result.erase(prev, pos - prev + 3);
    }
  }

  return result;
}

struct ncx_navpoint_state_t {
  int depth;
  std::string src;
  std::string title;
  int level;
  bool in_text;
  int text_depth;
};

struct ncx_parse_data_t {
  std::vector<toc_entry_t> *entries;
  std::string base_path;
  int depth;
  std::vector<ncx_navpoint_state_t> stack;
};

struct nav_parse_data_t {
  std::vector<toc_entry_t> *entries;
  std::string base_path;
  int depth;
  int toc_nav_depth;
  int nav_level;
  int anchor_depth;
  std::string current_href;
  std::string current_title;
  int current_level;
};

static void ncx_start(void *userdata, const char *el, const char **attr) {
  ncx_parse_data_t *d = (ncx_parse_data_t *)userdata;
  d->depth++;
  const char *lname = LocalName(el);

  if (!strcmp(lname, "navPoint")) {
    ncx_navpoint_state_t node;
    node.depth = d->depth;
    node.level = (int)d->stack.size();
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
      entry.level = (unsigned char)std::min(15, std::max(0, node.level));
      if (!entry.href.empty() && !entry.title.empty())
        d->entries->push_back(entry);
    }
  }

  d->depth--;
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

  if (d->toc_nav_depth > 0 && !strcmp(lname, "li")) {
    d->nav_level++;
  }

  if (d->toc_nav_depth > 0 && !strcmp(lname, "a")) {
    const char *href = AttrValue(attr, "href");
    if (href && *href) {
      d->anchor_depth = d->depth;
      d->current_href = ResolveRelativePath(d->base_path, href);
      d->current_level = (d->nav_level > 0) ? (d->nav_level - 1) : 0;
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
        entry.level = (unsigned char)std::min(15, std::max(0, d->current_level));
        d->entries->push_back(entry);
      }
    }
    d->anchor_depth = 0;
    d->current_href.clear();
    d->current_title.clear();
  }

  if (d->toc_nav_depth == d->depth && !strcmp(lname, "nav")) {
    d->toc_nav_depth = 0;
    d->nav_level = 0;
  }

  if (d->toc_nav_depth > 0 && !strcmp(lname, "li") && d->nav_level > 0) {
    d->nav_level--;
  }

  d->depth--;
}

} // namespace

namespace epub_ncx_parser {

bool ParseNcxWithExpat(const std::string &xml, const std::string &base_path,
                       std::vector<toc_entry_t> *entries) {
  if (xml.empty() || !entries)
    return false;
  if (xml.size() > EPUB_TOC_MAX_BYTES)
    return false;

  ncx_parse_data_t d;
  d.entries = entries;
  d.base_path = base_path;
  d.depth = 0;

  xml_parse_utils::XmlParserOptions options;
  options.start_element = ncx_start;
  options.end_element = ncx_end;
  options.character_data = ncx_char;
  options.user_data = &d;

  return xml_parse_utils::ParseXmlString(xml, options).ok && !entries->empty();
}

bool ParseNcxLightweight(const std::string &xml, const std::string &base_path,
                         std::vector<toc_entry_t> *entries) {
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
    entry.level = 0;
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
      std::string tag = lower.substr(content_pos, tag_end - content_pos + 1);
      size_t src_pos = tag.find("src=");
      if (src_pos != std::string::npos) {
        size_t q0 = tag.find('"', src_pos);
        if (q0 == std::string::npos)
          q0 = tag.find('\'', src_pos);
        if (q0 != std::string::npos) {
          size_t q1 = tag.find(tag[q0], q0 + 1);
          if (q1 != std::string::npos)
            pending_src = xml.substr(content_pos + q0 + 1, q1 - q0 - 1);
        }
      }
      pos = tag_end + 1;
    } else {
      size_t tag_end = lower.find('>', text_pos);
      if (tag_end == std::string::npos)
        break;
      size_t close_pos = lower.find("</text", tag_end);
      if (close_pos == std::string::npos)
        break;
      pending_title = xml.substr(tag_end + 1, close_pos - tag_end - 1);
      try_flush();
      pos = close_pos;
      size_t gt = lower.find('>', pos);
      if (gt == std::string::npos)
        break;
      pos = gt + 1;
    }
  }

  try_flush();
  return any;
}

bool ParseNavWithExpat(const std::string &xml, const std::string &base_path,
                       std::vector<toc_entry_t> *entries) {
  if (xml.empty() || !entries)
    return false;
  if (xml.size() > EPUB_TOC_MAX_BYTES)
    return false;

  nav_parse_data_t d;
  d.entries = entries;
  d.base_path = base_path;
  d.depth = 0;
  d.toc_nav_depth = 0;
  d.nav_level = 0;
  d.anchor_depth = 0;

  xml_parse_utils::XmlParserOptions options;
  options.start_element = nav_start;
  options.end_element = nav_end;
  options.character_data = nav_char;
  options.user_data = &d;

  return xml_parse_utils::ParseXmlString(xml, options).ok && !entries->empty();
}

} // namespace epub_ncx_parser
