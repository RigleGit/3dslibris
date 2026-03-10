/*
    3dslibris - book.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Core book state/container logic shared across EPUB/FB2/TXT/RTF/ODT.
    - Chapter/bookmark management and TOC target resolution helpers.
    - Page ownership/lifetime and parser integration points.
*/

#include "book.h"

#include "app.h"
#include "main.h"
#include "parse.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

// extern App *app;

namespace {

static bool EqualsAsciiNoCase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static std::string BasenamePathLocal(const std::string &path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return "";
  return path.substr(slash + 1);
}

static std::string AnchorTokenKey(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      out.push_back((char)c);
  }
  return out;
}

static std::string AnchorDigits(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= '0' && c <= '9')
      out.push_back((char)c);
  }
  return out;
}

static std::string NormalizePathForAnchor(const std::string &path) {
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

static std::string UrlDecodeComponent(const std::string &input) {
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

static std::string ToLowerAsciiLocal(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static std::string BuildAnchorKey(const std::string &docpath,
                                  const std::string &anchor_raw) {
  if (docpath.empty() || anchor_raw.empty())
    return "";

  std::string doc = UrlDecodeComponent(docpath);
  std::string anchor = UrlDecodeComponent(anchor_raw);
  while (!anchor.empty() && anchor[0] == '#')
    anchor.erase(anchor.begin());
  if (anchor.empty())
    return "";
  if (anchor.size() > 512)
    anchor.resize(512);

  std::string key = NormalizePathForAnchor(doc);
  if (key.empty())
    return "";
  key.push_back('#');
  key += anchor;
  return key;
}

static std::string NormalizeAnchorHrefKey(const std::string &href) {
  if (href.empty())
    return "";
  std::string decoded = UrlDecodeComponent(href);
  size_t hash = decoded.find('#');
  if (hash == std::string::npos || hash + 1 >= decoded.size())
    return "";
  std::string path = decoded.substr(0, hash);
  std::string anchor = decoded.substr(hash + 1);
  size_t q = anchor.find('?');
  if (q != std::string::npos)
    anchor = anchor.substr(0, q);
  return BuildAnchorKey(path, anchor);
}

} // namespace

namespace xml::book::metadata {

std::string title;

void start(void *userdata, const char *el, const char **attr) {
  //! Expat callback, when entering an element.
  //! For finding book title only.

  if (!strcmp(el, "title")) {
    parse_push((parsedata_t *)userdata, TAG_TITLE);
  }
}

void chardata(void *userdata, const char *txt, int txtlen) {
  //! Expat callback, when in char data for element.
  //! For finding book title only.

  if (!parse_in((parsedata_t *)userdata, TAG_TITLE))
    return;
  title = txt;
}

void end(void *userdata, const char *el) {
  //! Expat callback, when exiting an element.
  //! For finding book title only.

  parsedata_t *data = (parsedata_t *)userdata;
  if (!strcmp(el, "title"))
    data->book->SetTitle(title.c_str());
  if (!strcmp(el, "head"))
    data->status = 1; // done.
  parse_pop(data);
}

} // namespace xml::book::metadata

namespace xml::book {

void chardata(void *data, const XML_Char *txt, int txtlen);
static const size_t kFb2BinaryMaxChars = 6 * 1024 * 1024;

static std::string NormalizeDocPath(const std::string &path) {
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

static bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static bool PathLooksLikeTocDoc(const std::string &path) {
  if (path.empty())
    return false;
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return lower.find("toc") != std::string::npos ||
         lower.find("indice") != std::string::npos ||
         lower.find("index") != std::string::npos ||
         lower.find("contents") != std::string::npos ||
         lower.find("contenido") != std::string::npos ||
         lower.find("nav") != std::string::npos;
}

static std::string ResolveDocPath(const std::string &base_doc_path,
                                  const std::string &href) {
  if (href.empty())
    return "";
  if (href.find("://") != std::string::npos)
    return "";
  if (href.compare(0, 5, "data:") == 0)
    return "";

  std::string clean_href = href;
  size_t hash = clean_href.find('#');
  if (hash != std::string::npos)
    clean_href = clean_href.substr(0, hash);
  if (clean_href.empty())
    return "";

  if (!clean_href.empty() && clean_href[0] == '/')
    return NormalizeDocPath(clean_href);

  std::string base = base_doc_path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos)
    base = base.substr(0, slash + 1);
  else
    base.clear();

  return NormalizeDocPath(base + clean_href);
}

static std::string NormalizeFb2ChapterTitle(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty())
      out.push_back(' ');
    pending_space = false;
    out.push_back((char)c);
  }
  return out;
}

void linefeed(parsedata_t *p) {
  p->buf[p->buflen++] = '\n';
  p->pen.x = MARGINLEFT;
  p->pen.y += p->app->ts->GetHeight() + p->app->ts->linespacing;
  p->linebegan = false;
}

bool blankline(parsedata_t *p) {
  // Was the preceding text a blank line?
  if (p->buflen < 3)
    return true;
  return (p->buf[p->buflen - 1] == '\n') && (p->buf[p->buflen - 2] == '\n');
}

void instruction(void *data, const char *target, const char *pidata) {}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  auto app = p->app;

  if (p->fb2_mode && parse_in(p, TAG_BODY)) {
    if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth < 31)
        p->fb2_section_depth++;
      if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
        p->fb2_section_has_chapter[p->fb2_section_depth] = false;
    } else if (XmlNameEquals(el, "title") && p->fb2_section_depth > 0) {
      p->fb2_title_depth++;
      if (p->fb2_title_depth == 1 && p->fb2_title_capture_depth == 0 &&
          p->fb2_section_depth < 32 &&
          !p->fb2_section_has_chapter[p->fb2_section_depth]) {
        p->fb2_title_capture_depth = p->fb2_section_depth;
        p->fb2_title_text.clear();
      }
    }
  }

  // Register named anchors while parsing EPUB documents so TOC hrefs with
  // fragments (#id) can jump to the closest real page instead of chapter start.
  if (p->book && !p->docpath.empty() && attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (XmlNameEquals(attr[i], "id") || XmlNameEquals(attr[i], "name")) {
        p->book->AddChapterAnchor(p->docpath, attr[i + 1]);
      }
    }
  }

  if (!strcmp(el, "html"))
    parse_push(p, TAG_HTML);
  else if (!strcmp(el, "body"))
    parse_push(p, TAG_BODY);
  else if (!strcmp(el, "div"))
    parse_push(p, TAG_DIV);
  else if (!strcmp(el, "dt"))
    parse_push(p, TAG_DT);
  else if (!strcmp(el, "h1")) {
    parse_push(p, TAG_H1);
    bool lf = !blankline(p);
    p->buf[p->buflen] = TEXT_BOLD_ON;
    p->buflen++;
    p->pos++;
    p->bold = true;
    if (lf)
      linefeed(p);
  } else if (!strcmp(el, "h2")) {
    parse_push(p, TAG_H2);
    bool lf = !blankline(p);
    p->buf[p->buflen] = TEXT_BOLD_ON;
    p->buflen++;
    p->pos++;
    p->bold = true;
    if (lf)
      linefeed(p);
  } else if (!strcmp(el, "h3")) {
    parse_push(p, TAG_H3);
    linefeed(p);
  } else if (!strcmp(el, "h4")) {
    parse_push(p, TAG_H4);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "h5")) {
    parse_push(p, TAG_H5);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "h6")) {
    parse_push(p, TAG_H6);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "head"))
    parse_push(p, TAG_HEAD);
  else if (!strcmp(el, "ol"))
    parse_push(p, TAG_OL);
  else if (!strcmp(el, "p")) {
    parse_push(p, TAG_P);
    if (!blankline(p)) {
      for (int i = 0; i < p->app->paraspacing; i++) {
        linefeed(p);
      }
      for (int i = 0; i < p->app->paraindent; i++) {
        p->buf[p->buflen++] = ' ';
        p->pen.x += p->app->ts->GetAdvance(' ');
      }
    }
  } else if (!strcmp(el, "pre"))
    parse_push(p, TAG_PRE);
  else if (!strcmp(el, "script"))
    parse_push(p, TAG_SCRIPT);
  else if (!strcmp(el, "style"))
    parse_push(p, TAG_STYLE);
  else if (XmlNameEquals(el, "title"))
    parse_push(p, TAG_TITLE);
  else if (!strcmp(el, "td"))
    parse_push(p, TAG_TD);
  else if (!strcmp(el, "ul"))
    parse_push(p, TAG_UL);
  else if (!strcmp(el, "strong") || !strcmp(el, "b")) {
    parse_push(p, TAG_STRONG);
    p->buf[p->buflen] = TEXT_BOLD_ON;
    p->buflen++;
    p->pos++;
    p->bold = true;
    if (p->italic) {
      app->ts->SetStyle(TEXT_STYLE_BOLDITALIC);
    } else {
      app->ts->SetStyle(TEXT_STYLE_BOLD);
    }
  } else if (!strcmp(el, "em") || !strcmp(el, "i")) {
    parse_push(p, TAG_EM);
    p->buf[p->buflen] = TEXT_ITALIC_ON;
    p->buflen++;
    p->italic = true;
    if (p->bold) {
      app->ts->SetStyle(TEXT_STYLE_BOLDITALIC);
    } else {
      app->ts->SetStyle(TEXT_STYLE_ITALIC);
    }
  } else if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
    parse_push(p, TAG_UNKNOWN);

    const char *src = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (XmlNameEquals(attr[i], "src") || XmlNameEquals(attr[i], "href")) {
        src = attr[i + 1];
      }
    }

    std::string resolved;
    if (src && *src) {
      std::string raw_src(src);
      if (!raw_src.empty() && raw_src[0] == '#') {
        // FB2 inline binary reference (<image href="#id">).
        resolved = "fb2:" + raw_src.substr(1);
      } else {
        resolved = ResolveDocPath(p->docpath, raw_src);
      }
    }

    if (!resolved.empty() && p->book) {
      u16 image_id = p->book->RegisterInlineImage(resolved);

      // Inline image as one-screen block: token + id.
      if (p->buflen + 4 < PAGEBUFSIZE) {
        if (!blankline(p))
          linefeed(p);
        p->buf[p->buflen++] = TEXT_IMAGE;
        p->buf[p->buflen++] = (u8)((image_id >> 8) & 0xFF);
        p->buf[p->buflen++] = (u8)(image_id & 0xFF);
        p->buf[p->buflen++] = '\n';
      }

      // Reserve a full screen slot for this image.
      int maxHeight = (p->screen == 1) ? 320 : 400;
      p->pen.x = app->ts->margin.left;
      p->pen.y = maxHeight - 1;
      p->linebegan = false;
    } else {
      // Keep a lightweight fallback marker when src cannot be resolved.
      const char *fallback = "[illustration]";
      if (!blankline(p))
        linefeed(p);
      chardata(p, fallback, (int)strlen(fallback));
      linefeed(p);
    }
  } else if (XmlNameEquals(el, "binary")) {
    parse_push(p, TAG_UNKNOWN);

    p->collecting_fb2_binary = false;
    p->fb2_binary_too_large = false;
    p->fb2_binary_id.clear();
    p->fb2_binary_data.clear();

    const char *id = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (XmlNameEquals(attr[i], "id")) {
        id = attr[i + 1];
      }
    }

    if (id && *id && p->book) {
      p->collecting_fb2_binary = true;
      p->fb2_binary_id = id;
      if (!p->fb2_binary_id.empty() && p->fb2_binary_id[0] == '#')
        p->fb2_binary_id.erase(0, 1);
    }
  } else
    parse_push(p, TAG_UNKNOWN);
}

void chardata(void *data, const XML_Char *txt, int txtlen) {
  //! reflow text on the fly, into page data structure.

  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

  if (p->collecting_fb2_binary) {
    if (!p->fb2_binary_too_large) {
      for (int i = 0; i < txtlen; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          continue;
        p->fb2_binary_data.push_back((char)c);
      }
      if (p->fb2_binary_data.size() > kFb2BinaryMaxChars) {
        p->fb2_binary_data.clear();
        p->fb2_binary_too_large = true;
      }
    }
    return;
  }

  if (parse_in(p, TAG_TITLE)) {
    if (p->fb2_mode && p->fb2_title_capture_depth > 0) {
      p->fb2_title_text.append((const char *)txt, txtlen);
    } else {
      p->doc_title.append((const char *)txt, txtlen);
    }
    return;
  }
  if (parse_in(p, TAG_SCRIPT))
    return;
  if (parse_in(p, TAG_STYLE))
    return;
  if ((parse_in(p, TAG_H1) || parse_in(p, TAG_H2) || parse_in(p, TAG_H3)) &&
      p->doc_heading.size() < 160) {
    p->doc_heading.append((const char *)txt, txtlen);
  }

  int lineheight = ts->GetHeight();
  int linespacing = ts->linespacing;
  int spaceadvance = ts->GetAdvance((u16)' ');

  if (p->buflen == 0) {
    /** starting a new page. **/
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + lineheight;
    p->linebegan = false;
  }

  u8 advance = 0;
  int i = 0, j = 0;
  while (i < txtlen) {
    if (txt[i] == '\r') {
      i++;
      continue;
    }

    if (iswhitespace(txt[i])) {
      if (parse_in(p, TAG_PRE)) {
        p->buf[p->buflen++] = txt[i];
        if (txt[i] == '\n') {
          p->pen.x = ts->margin.left;
          p->pen.y += (lineheight + linespacing);
        } else {
          p->pen.x += spaceadvance;
        }
      } else if (p->linebegan && p->buflen &&
                 !iswhitespace(p->buf[p->buflen - 1])) {
        p->buf[p->buflen++] = ' ';
        p->pen.x += spaceadvance;
      }
      i++;
    } else {
      p->linebegan = true;
      advance = 0;
      u8 bytes = 1;
      for (j = i; (j < txtlen) && (!iswhitespace(txt[j])); j += bytes) {
        /** set type until the end of the next word.
            account for UTF-8 characters when advancing. **/
        u32 code = txt[j];
        bytes = 1;
        if (code >> 7) {
          // FIXME the performance bottleneck
          bytes = ts->GetCharCode((char *)&(txt[j]), &code);
        }

        advance += ts->GetAdvance(code);
        if (advance > ts->display.width - ts->margin.right - ts->margin.left) {
          // here's a line-long word, need to break it now.
          break;
        }
      }
    }

    if ((p->pen.x + advance) > (ts->display.width - ts->margin.right)) {
      // we overran the margin, insert a break.
      p->buf[p->buflen++] = '\n';
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
    }

    int maxHeight = (p->screen == 1) ? 320 : 400;
    int bottomMargin = (p->screen == 1) ? MIN(ts->margin.bottom, 16)
                                        : ts->margin.bottom;
    if ((p->pen.y + lineheight) > (maxHeight - bottomMargin)) {
      // reached bottom of screen.
      if (p->screen == 1) {
        // page full.
        // put chars into current page.
        Page *page = p->book->AppendPage();
        page->SetBuffer(p->buf, p->buflen);
        page->start = p->pos;
        p->pos += p->buflen;
        page->end = p->pos;
        p->pagecount++;

        // make a new page.
        p->buflen = 0;
        if (p->italic)
          p->buf[p->buflen++] = TEXT_ITALIC_ON;
        if (p->bold)
          p->buf[p->buflen++] = TEXT_BOLD_ON;
        p->screen = 0;
      } else
        // move to right screen.
        p->screen = 1;

      p->pen.x = ts->margin.left;
      p->pen.y = ts->margin.top + lineheight;
    }

    /** append this word to the page.
            chars stay UTF-8 until they are rendered. **/

    for (; i < j; i++) {
      if (iswhitespace(txt[i])) {
        if (p->linebegan) {
          p->buf[p->buflen] = ' ';
          p->buflen++;
        }
      } else {
        p->linebegan = true;
        p->buf[p->buflen] = txt[i];
        p->buflen++;
      }
    }
    p->pen.x += advance;
    advance = 0;
  }
}

void end(void *data, const char *el) {
  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

  if (XmlNameEquals(el, "binary")) {
    if (p->collecting_fb2_binary && !p->fb2_binary_too_large && p->book &&
        !p->fb2_binary_id.empty() && !p->fb2_binary_data.empty()) {
      p->book->StoreFb2InlineImage(p->fb2_binary_id, p->fb2_binary_data);
    }
    p->collecting_fb2_binary = false;
    p->fb2_binary_too_large = false;
    p->fb2_binary_id.clear();
    p->fb2_binary_data.clear();
    parse_pop(p);
    return;
  }

  if (p->fb2_mode) {
    if (XmlNameEquals(el, "title")) {
      if (p->fb2_title_depth > 0) {
        bool finishing_capture = (p->fb2_title_depth == 1 &&
                                  p->fb2_title_capture_depth > 0 &&
                                  p->fb2_title_capture_depth ==
                                      p->fb2_section_depth);
        if (finishing_capture && p->book) {
          std::string chapter_title = NormalizeFb2ChapterTitle(p->fb2_title_text);
          if (!chapter_title.empty()) {
            int level = p->fb2_section_depth > 0 ? p->fb2_section_depth - 1 : 0;
            if (level > 255)
              level = 255;
            p->book->AddChapter(p->book->GetPageCount(), chapter_title,
                                (u8)level);
            if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
              p->fb2_section_has_chapter[p->fb2_section_depth] = true;
          }
          p->fb2_title_text.clear();
          p->fb2_title_capture_depth = 0;
        }
        p->fb2_title_depth--;
        if (p->fb2_title_depth < 0)
          p->fb2_title_depth = 0;
      }
    } else if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth > 0) {
        if (p->fb2_section_depth < 32)
          p->fb2_section_has_chapter[p->fb2_section_depth] = false;
        p->fb2_section_depth--;
      }
      if (p->fb2_section_depth < 0)
        p->fb2_section_depth = 0;
      if (p->fb2_title_capture_depth > p->fb2_section_depth) {
        p->fb2_title_capture_depth = 0;
        p->fb2_title_text.clear();
      }
    }
  }

  if (!strcmp(el, "body")) {
    // Save off our last page.
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    p->buflen = 0;
    // Retain styles across the page.
    if (p->italic)
      p->buf[p->buflen++] = TEXT_ITALIC_ON;
    if (p->bold)
      p->buf[p->buflen++] = TEXT_BOLD_ON;
    parse_pop(p);
    return;
  }

  if (!strcmp(el, "br")) {
    linefeed(p);
  } else if (!strcmp(el, "a")) {
    // Many EPUB TOC/Nav documents are built as dense anchor lists with little
    // structural markup; force line breaks there to keep the reading view sane.
    if (PathLooksLikeTocDoc(p->docpath) && p->linebegan && p->buflen > 0 &&
        p->buf[p->buflen - 1] != '\n') {
      linefeed(p);
    }
  } else if (!strcmp(el, "p")) {
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "div")) {
  } else if (!strcmp(el, "strong") || !strcmp(el, "b")) {
    p->buf[p->buflen] = TEXT_BOLD_OFF;
    p->buflen++;
    p->bold = false;
    if (p->italic) {
      ts->SetStyle(TEXT_STYLE_ITALIC);
    } else {
      ts->SetStyle(TEXT_STYLE_REGULAR);
    }
  } else if (!strcmp(el, "em") || !strcmp(el, "i")) {
    p->buf[p->buflen] = TEXT_ITALIC_OFF;
    p->buflen++;
    p->italic = false;
    if (p->bold) {
      ts->SetStyle(TEXT_STYLE_BOLD);
    } else {
      ts->SetStyle(TEXT_STYLE_REGULAR);
    }
  } else if (!strcmp(el, "h1")) {
    p->buf[p->buflen] = TEXT_BOLD_OFF;
    p->buflen++;
    p->bold = false;
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "h2")) {
    p->buf[p->buflen] = TEXT_BOLD_OFF;
    p->buflen++;
    p->bold = false;
    linefeed(p);
  } else if (!strcmp(el, "h3") || !strcmp(el, "h4") || !strcmp(el, "h5") ||
             !strcmp(el, "h6") || !strcmp(el, "hr") || !strcmp(el, "pre")) {
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "li") || !strcmp(el, "ul") || !strcmp(el, "ol")) {
    linefeed(p);
  }

  parse_pop(p);

  int maxHeight = (p->screen == 1) ? 320 : 400;
  int bottomMargin =
      (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
  int lineheight = p->app->ts->GetHeight();
  if ((p->pen.y + lineheight) > (maxHeight - bottomMargin)) {
    if (p->screen == 1) {
      // End of right screen; end of page.
      // Copy in buffered char data into a new page.
      Page *page = p->book->AppendPage();
      page->SetBuffer(p->buf, p->buflen);
      p->buflen = 0;
      if (p->italic)
        p->buf[p->buflen++] = TEXT_ITALIC_ON;
      if (p->bold)
        p->buf[p->buflen++] = TEXT_BOLD_ON;
      p->screen = 0;
    } else
      // End of left screen; same page, next screen.
      p->screen = 1;
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + ts->GetHeight();
    p->linebegan = false;
  }
}

int unknown(void *encodingHandlerData, const XML_Char *name,
            XML_Encoding *info) {
  return 0;
}

void fallback(void *data, const XML_Char *s, int len) {
  // Handles HTML entities in body text.

  parsedata_t *p = (parsedata_t *)data;
  int advancespace = p->app->ts->GetAdvance(' ');
  if (s[0] == '&') {
    /** if it's decimal, convert the UTF-16 to UTF-8. */
    int code = 0;
    sscanf(s, "&#%d;", &code);
    if (code) {
      if (code >= 128 && code <= 2047) {
        p->buf[p->buflen++] = 192 + (code / 64);
        p->buf[p->buflen++] = 128 + (code % 64);
      } else if (code >= 2048 && code <= 65535) {
        p->buf[p->buflen++] = 224 + (code / 4096);
        p->buf[p->buflen++] = 128 + ((code / 64) % 64);
        p->buf[p->buflen++] = 128 + (code % 64);
      }
      // TODO - support 4-byte codes

      p->pen.x += p->app->ts->GetAdvance(code);
      return;
    }

    /** otherwise, handle only common HTML named entities. */
    if (!strncmp(s, "&nbsp;", 5)) {
      p->buf[p->buflen++] = ' ';
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&quot;")) {
      p->buf[p->buflen++] = '"';
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&amp;")) {
      p->buf[p->buflen++] = '&';
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&lt;")) {
      p->buf[p->buflen++] = '<';
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&gt;")) {
      p->buf[p->buflen++] = '>';
      p->pen.x += advancespace;
      return;
    }
  }
}

} // namespace xml::book

Book::Book(App *a) {
  foldername.clear();
  filename.clear();
  title.clear();
  author.clear();
  browser_display_name_cache.clear();
  browser_display_name_cached = false;
  pages.clear();
  position = 0;
  format = FORMAT_UNDEF;
  app = a;
  coverPixels = nullptr;
  coverWidth = 0;
  coverHeight = 0;
  coverImagePath.clear();
  coverTried = false;
  metadataIndexTried = false;
  metadataIndexed = false;
  tocResolveTried = false;
  tocResolved = false;
  fb2_inline_images_bytes = 0;
  inline_image_cache_bytes = 0;
  ClearTocConfidence();
}

Book::~Book() {
  Close();
  if (coverPixels) {
    delete[] coverPixels;
    coverPixels = nullptr;
  }
}

void Book::SetFolderName(const char *name) {
  foldername.clear();
  foldername = name;
}

void Book::SetFileName(const char *name) {
  filename.clear();
  filename = name;
  ClearBrowserDisplayNameCache();
}

void Book::SetTitle(const char *name) {
  title.clear();
  title = name;
}

void Book::SetAuthor(std::string &name) {
  author.clear();
  author = name;
}

void Book::SetFolderName(std::string &name) { foldername = name; }

std::list<u16> *Book::GetBookmarks() { return &bookmarks; }
const std::vector<ChapterEntry> &Book::GetChapters() const { return chapters; }

void Book::AddChapterAnchor(const std::string &docpath,
                            const std::string &anchor_id) {
  if (docpath.empty() || anchor_id.empty())
    return;
  if (chapter_anchor_pages.size() >= 8192)
    return;

  std::string key = BuildAnchorKey(docpath, anchor_id);
  if (key.empty())
    return;

  if (chapter_anchor_pages.find(key) == chapter_anchor_pages.end()) {
    chapter_anchor_pages[key] = GetPageCount();
  }
}

bool Book::FindChapterAnchorPage(const std::string &href, u16 *page_out) const {
  if (!page_out)
    return false;
  std::string key = NormalizeAnchorHrefKey(href);
  if (key.empty())
    return false;

  auto hit = chapter_anchor_pages.find(key);
  if (hit != chapter_anchor_pages.end()) {
    *page_out = hit->second;
    return true;
  }

  // Fallback only for malformed files with inconsistent anchor case.
  std::string key_lc = ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_anchor_pages) {
    if (ToLowerAsciiLocal(kv.first) == key_lc) {
      *page_out = kv.second;
      return true;
    }
  }

  // Robust fallback for malformed EPUBs where TOC path and parsed doc path differ
  // but anchor IDs are still consistent.
  size_t hash = key.find('#');
  if (hash != std::string::npos && hash + 1 < key.size()) {
    std::string key_path_lc = ToLowerAsciiLocal(key.substr(0, hash));
    std::string key_base_lc = ToLowerAsciiLocal(BasenamePathLocal(key_path_lc));
    std::string key_anchor_lc = ToLowerAsciiLocal(key.substr(hash + 1));
    u16 target_doc_page = 0;
    bool has_target_doc = FindChapterDocStartPage(key_path_lc, &target_doc_page);

    bool path_anchor_found = false;
    u16 path_anchor_page = 0;
    bool path_anchor_ambiguous = false;
    bool base_anchor_found = false;
    u16 base_anchor_page = 0;
    bool base_anchor_ambiguous = false;
    bool anchor_found = false;
    u16 anchor_page = 0;
    bool anchor_ambiguous = false;
    bool fuzzy_doc_found = false;
    u16 fuzzy_doc_page = 0;
    bool fuzzy_doc_ambiguous = false;
    const std::string key_token = AnchorTokenKey(key_anchor_lc);
    const std::string key_digits = AnchorDigits(key_anchor_lc);

    for (const auto &kv : chapter_anchor_pages) {
      size_t kv_hash = kv.first.find('#');
      if (kv_hash == std::string::npos || kv_hash + 1 >= kv.first.size())
        continue;
      std::string kv_path_lc = ToLowerAsciiLocal(kv.first.substr(0, kv_hash));
      std::string kv_base_lc = ToLowerAsciiLocal(BasenamePathLocal(kv_path_lc));
      std::string kv_anchor_lc = ToLowerAsciiLocal(kv.first.substr(kv_hash + 1));
      bool exact_anchor = (kv_anchor_lc == key_anchor_lc);

      if (exact_anchor) {
        if (!anchor_found) {
          anchor_found = true;
          anchor_page = kv.second;
        } else if (anchor_page != kv.second) {
          anchor_ambiguous = true;
        }

        if (has_target_doc) {
          u16 candidate_doc_page = 0;
          if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
              candidate_doc_page == target_doc_page) {
            if (!path_anchor_found) {
              path_anchor_found = true;
              path_anchor_page = kv.second;
            } else if (path_anchor_page != kv.second) {
              path_anchor_ambiguous = true;
            }
          }
        }

        if (!key_base_lc.empty() && kv_base_lc == key_base_lc) {
          if (!base_anchor_found) {
            base_anchor_found = true;
            base_anchor_page = kv.second;
          } else if (base_anchor_page != kv.second) {
            base_anchor_ambiguous = true;
          }
        }
      }

      if (has_target_doc && !path_anchor_found && !key_token.empty()) {
        u16 candidate_doc_page = 0;
        if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
            candidate_doc_page == target_doc_page) {
          std::string cand_token = AnchorTokenKey(kv_anchor_lc);
          std::string cand_digits = AnchorDigits(kv_anchor_lc);
          bool fuzzy_match = false;
          if (!cand_token.empty() && cand_token == key_token) {
            fuzzy_match = true;
          } else if (!key_digits.empty() && !cand_digits.empty() &&
                     cand_digits == key_digits) {
            fuzzy_match = true;
          } else if (!key_digits.empty() && !cand_token.empty() &&
                     cand_token.size() > key_digits.size() &&
                     cand_token.size() <= key_digits.size() + 4 &&
                     cand_token.compare(cand_token.size() - key_digits.size(),
                                       key_digits.size(),
                                       key_digits) == 0) {
            fuzzy_match = true;
          }

          if (fuzzy_match) {
            if (!fuzzy_doc_found) {
              fuzzy_doc_found = true;
              fuzzy_doc_page = kv.second;
            } else if (fuzzy_doc_page != kv.second) {
              fuzzy_doc_ambiguous = true;
            }
          }
        }
      }
    }

    if (path_anchor_found && !path_anchor_ambiguous) {
      *page_out = path_anchor_page;
      return true;
    }
    if (fuzzy_doc_found && !fuzzy_doc_ambiguous) {
      *page_out = fuzzy_doc_page;
      return true;
    }
    if (base_anchor_found && !base_anchor_ambiguous) {
      *page_out = base_anchor_page;
      return true;
    }
    if (anchor_found && !anchor_ambiguous) {
      *page_out = anchor_page;
      return true;
    }
  }

  return false;
}

size_t Book::GetChapterAnchorCount() const { return chapter_anchor_pages.size(); }

void Book::ClearChapterAnchors() { chapter_anchor_pages.clear(); }

void Book::SetChapterDocStartPage(const std::string &docpath, u16 page) {
  if (docpath.empty())
    return;
  std::string key = NormalizePathForAnchor(UrlDecodeComponent(docpath));
  if (key.empty())
    return;
  if (chapter_doc_start_pages.find(key) == chapter_doc_start_pages.end())
    chapter_doc_start_pages[key] = page;
}

bool Book::FindChapterDocStartPage(const std::string &href, u16 *page_out) const {
  if (!page_out)
    return false;
  if (href.empty())
    return false;

  std::string decoded = UrlDecodeComponent(href);
  size_t hash = decoded.find('#');
  if (hash != std::string::npos)
    decoded = decoded.substr(0, hash);
  size_t q = decoded.find('?');
  if (q != std::string::npos)
    decoded = decoded.substr(0, q);

  std::string key = NormalizePathForAnchor(decoded);
  if (key.empty())
    return false;

  auto hit = chapter_doc_start_pages.find(key);
  if (hit != chapter_doc_start_pages.end()) {
    *page_out = hit->second;
    return true;
  }

  std::string key_lc = ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_doc_start_pages) {
    if (ToLowerAsciiLocal(kv.first) == key_lc) {
      *page_out = kv.second;
      return true;
    }
  }

  return false;
}

const std::unordered_map<std::string, u16> &Book::GetChapterDocStartPages() const {
  return chapter_doc_start_pages;
}

void Book::ClearChapterDocStartPages() { chapter_doc_start_pages.clear(); }

void Book::AddChapter(u16 page, const std::string &title, u8 level) {
  ChapterEntry entry;
  entry.page = page;
  entry.level = level;
  entry.title = title;
  chapters.push_back(entry);
}

void Book::ClearChapters() { chapters.clear(); }

int Book::GetNextBookmark() {
  //! nyi
  return -1337;
}

int GotoNextBookmarkedPage() {
  //! nyi
  return -1337;
}

int Book::GetPreviousBookmark() {
  //! Return previous page index,
  //! relative to current one.
  //! nyi
  return -1337;
}

int GotoPreviousBookmarkedPage() { return -1337; }

int Book::GetPosition(int offset) {
  //! For the character offset, get the page.
  return -1337;
}

Page *Book::GetPage() { return pages[position]; }

Page *Book::GetPage(int index) { return pages[index]; }

u16 Book::GetPageCount() { return pages.size(); }

const char *Book::GetTitle() { return title.c_str(); }

const char *Book::GetFileName() { return filename.c_str(); }

const char *Book::GetFolderName() { return foldername.c_str(); }

int Book::GetPosition() { return position; }

void Book::SetPage(u16 index) { position = index; }

void Book::SetPosition(int pos) { position = pos; }

Page *Book::AppendPage() {
  Page *page = new Page(this);
  pages.push_back(page);
  return page;
}

Page *Book::AdvancePage() {
  if (position < (int)pages.size())
    position++;
  return GetPage();
}

Page *Book::RetreatPage() {
  if (position > 0)
    position--;
  return GetPage();
}

void Book::Close() {
  CancelDeferredMobiParse();
  std::vector<Page *>::iterator it = pages.begin();
  while (it != pages.end()) {
    delete *it;
    *it = nullptr;
    ++it;
  }
  pages.clear();
  chapters.clear();
  ClearChapterAnchors();
  ClearChapterDocStartPages();
  ClearInlineImages();
  ClearTocConfidence();
  // pages.erase(pages.begin(), pages.end());
}
