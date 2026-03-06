#include "book.h"

#include "app.h"
#include "main.h"
#include "parse.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

// extern App *app;

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
  if (!strcmp(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && !strcmp(colon + 1, needle));
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
  else if (!strcmp(el, "title"))
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
    p->doc_title.append((const char *)txt, txtlen);
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
  fb2_inline_images_bytes = 0;
  inline_image_cache_bytes = 0;
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

void Book::AddChapter(u16 page, const std::string &title) {
  ChapterEntry entry;
  entry.page = page;
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
  std::vector<Page *>::iterator it = pages.begin();
  while (it != pages.end()) {
    delete *it;
    *it = nullptr;
    ++it;
  }
  pages.clear();
  chapters.clear();
  ClearInlineImages();
  // pages.erase(pages.begin(), pages.end());
}
