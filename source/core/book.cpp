#include "book.h"

#include "app.h"
#include "epub.h"
#include "main.h"
#include "parse.h"
#include <errno.h>
#include <stdio.h>
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
  } else if (!strcmp(el, "img") || !strcmp(el, "image")) {
    parse_push(p, TAG_UNKNOWN);

    // dslibris has no inline bitmap renderer in page flow. Emit a readable
    // placeholder so image-only sections are not blank.
    const char *src = NULL;
    const char *alt = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (!strcmp(attr[i], "src") || !strcmp(attr[i], "href") ||
          !strcmp(attr[i], "xlink:href")) {
        src = attr[i + 1];
      } else if (!strcmp(attr[i], "alt")) {
        alt = attr[i + 1];
      }
    }

    std::string label = "[illustration";
    if (alt && *alt) {
      label += ": ";
      label += alt;
    } else if (src && *src) {
      std::string s(src);
      size_t slash = s.find_last_of('/');
      if (slash != std::string::npos)
        s = s.substr(slash + 1);
      label += ": ";
      label += s;
    }
    label += "]";

    if (!blankline(p))
      linefeed(p);
    chardata(p, label.c_str(), (int)label.size());
    linefeed(p);
  } else
    parse_push(p, TAG_UNKNOWN);
}

void chardata(void *data, const XML_Char *txt, int txtlen) {
  //! reflow text on the fly, into page data structure.

  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

  if (parse_in(p, TAG_TITLE))
    return;
  if (parse_in(p, TAG_SCRIPT))
    return;
  if (parse_in(p, TAG_STYLE))
    return;

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

void Book::Cache() {
  FILE *fp = fopen("/cache.dat", "w");
  if (!fp)
    return;

  int buflen = 0;
  int pagecount = GetPageCount();
  fprintf(fp, "%d\n", pagecount);
  for (int i = 0; i < pagecount; i++) {
    buflen += GetPage(i)->GetLength();
    fprintf(fp, "%d\n", buflen);
    GetPage(i)->Cache(fp);
  }
  fclose(fp);
}

u8 Book::Open() {
  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  char logmsg[256];
  sprintf(logmsg, "Opening: %s", path.c_str());
  app->PrintStatus(logmsg);

  // Page layout is a function of the current style.
  app->ts->SetStyle(TEXT_STYLE_REGULAR);
  u8 err = epub(this, path, false);
  if (!err)
    if (position > (int)pages.size())
      position = pages.size() - 1;
  return err;
}

u8 Book::Index() {
  if (metadataIndexTried)
    return metadataIndexed ? 0 : 1;
  metadataIndexTried = true;

  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());
  int err = epub(this, path, true);
  if (!err) {
    metadataIndexed = true;
  }
  return err;
}

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char *filebuf = new char[BUFSIZE];
  if (!filebuf) {
    rc = 1;
    return (rc);
  }

  char path[MAXPATHLEN];
  sprintf(path, "%s%s", GetFolderName(), GetFileName());
  FILE *fp = fopen(path, "r");
  if (!fp) {
    delete[] filebuf;
    rc = 255;
    return (rc);
  }

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.cachefile = fopen("/cache.dat", "w");
  parsedata.book = this;

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    delete[] filebuf;
    fclose(fp);
    rc = 253;
    return rc;
  }
  XML_ParserReset(p, NULL);
  XML_SetUserData(p, &parsedata);
  XML_SetDefaultHandler(p, xml::book::fallback);
  XML_SetProcessingInstructionHandler(p, xml::book::instruction);
  XML_SetElementHandler(p, xml::book::start, xml::book::end);
  XML_SetCharacterDataHandler(p, xml::book::chardata);
  if (!fulltext) {
    XML_SetElementHandler(p, xml::book::metadata::start,
                          xml::book::metadata::end);
    XML_SetCharacterDataHandler(p, xml::book::metadata::chardata);
  }

  enum XML_Status status;
  while (true) {
    int bytes_read = fread(filebuf, 1, BUFSIZE, fp);
    status = XML_Parse(p, filebuf, bytes_read, (bytes_read == 0));
    if (status == XML_STATUS_ERROR) {
      parse_error(p);
      rc = 254;
      break;
    }
    if (parsedata.status)
      break; // non-fulltext parsing signals it is done.
    if (bytes_read == 0)
      break; // assume our buffer ran out.
  }

  XML_ParserFree(p);
  fclose(fp);
  delete[] filebuf;

  return (rc);
}

void Book::Restore() {
  FILE *fp = fopen("/cache.dat", "r");
  if (!fp)
    return;

  int len, pagecount;
  u8 buf[BUFSIZE];

  fscanf(fp, "%d\n", &pagecount);
  for (int i = 0; i < pagecount - 1; i++) {
    fscanf(fp, "%d\n", &len);
    fread(buf, sizeof(char), len, fp);
    GetPage(i)->SetBuffer(buf, len);
  }
  fclose(fp);
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
  // pages.erase(pages.begin(), pages.end());
}
