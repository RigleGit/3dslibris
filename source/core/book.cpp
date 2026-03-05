#include "book.h"

#include "app.h"
#include "epub.h"
#include "main.h"
#include "parse.h"
#include "stb_image.h"
#include "unzip.h"
#include <algorithm>
#include <errno.h>
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
static const size_t kInlineImageCacheMaxBytes = 1024 * 1024;

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

    std::string resolved =
        (src && *src) ? ResolveDocPath(p->docpath, std::string(src)) : "";
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

u16 Book::RegisterInlineImage(const std::string &path) {
  if (path.empty())
    return 0;
  for (u16 i = 0; i < inline_images.size(); i++) {
    if (inline_images[i] == path)
      return i;
  }
  if (inline_images.size() >= 65535)
    return 0;
  inline_images.push_back(path);
  return (u16)(inline_images.size() - 1);
}

const std::string *Book::GetInlineImagePath(u16 id) const {
  if (id >= inline_images.size())
    return NULL;
  return &inline_images[id];
}

void Book::ClearInlineImageCache() {
  inline_image_cache.clear();
  inline_image_cache_bytes = 0;
}

void Book::ClearInlineImages() {
  inline_images.clear();
  ClearInlineImageCache();
}

bool Book::DrawInlineImage(Text *ts, u16 image_id) {
  if (!ts)
    return false;
  const std::string *image_path = GetInlineImagePath(image_id);
  if (!image_path || image_path->empty())
    return false;

  const bool left_screen = (ts->GetScreen() == ts->screenleft);
  const int screen_w = 240;
  const int screen_h = left_screen ? 400 : 320;
  const int pad = 2;
  const int avail_w = screen_w - (pad * 2);
  const int avail_h = screen_h - (pad * 2);

  auto blit_cached = [&](const InlineImageCacheEntry &entry) {
    u16 *dst = ts->GetScreen();
    const int stride = ts->display.height;
    for (int y = 0; y < entry.height; y++) {
      int dy = entry.start_y + y;
      if (dy < 0 || dy >= screen_h)
        continue;

      int draw_x = entry.start_x;
      int draw_w = entry.width;
      int src_x = 0;
      if (draw_x < 0) {
        src_x = -draw_x;
        draw_w -= src_x;
        draw_x = 0;
      }
      if (draw_x + draw_w > screen_w)
        draw_w = screen_w - draw_x;
      if (draw_w <= 0)
        continue;

      const u16 *src_row = entry.pixels.data() + (y * entry.width) + src_x;
      u16 *dst_row = dst + (dy * stride) + draw_x;
      memcpy(dst_row, src_row, draw_w * sizeof(u16));
    }
  };

  for (std::list<InlineImageCacheEntry>::iterator it = inline_image_cache.begin();
       it != inline_image_cache.end(); ++it) {
    if (it->image_id == image_id && it->screen_h == (u16)screen_h) {
      inline_image_cache.splice(inline_image_cache.begin(), inline_image_cache, it);
      blit_cached(inline_image_cache.front());
      return true;
    }
  }

  std::string epubpath = foldername + "/" + filename;
  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return false;

  int rc = unzLocateFile(uf, image_path->c_str(), 2);
  if (rc != UNZ_OK) {
    unzClose(uf);
    return false;
  }
  if (unzOpenCurrentFile(uf) != UNZ_OK) {
    unzClose(uf);
    return false;
  }

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
      fi.uncompressed_size == 0 || fi.uncompressed_size > (4 * 1024 * 1024)) {
    unzCloseCurrentFile(uf);
    unzClose(uf);
    return false;
  }

  std::vector<u8> compressed(fi.uncompressed_size);
  int bytes_read = unzReadCurrentFile(uf, compressed.data(), fi.uncompressed_size);
  unzCloseCurrentFile(uf);
  unzClose(uf);
  if (bytes_read <= 0)
    return false;

  int info_w = 0, info_h = 0, info_c = 0;
  if (!stbi_info_from_memory(compressed.data(), bytes_read, &info_w, &info_h,
                             &info_c))
    return false;
  if (info_w <= 0 || info_h <= 0)
    return false;
  if ((long long)info_w * (long long)info_h > 1500000LL)
    return false;

  int imgW = 0, imgH = 0, channels = 0;
  unsigned char *pixels = stbi_load_from_memory(
      compressed.data(), bytes_read, &imgW, &imgH, &channels, 4);
  if (!pixels)
    return false;

  float sx = (float)avail_w / (float)imgW;
  float sy = (float)avail_h / (float)imgH;
  float scale = (sx < sy) ? sx : sy;
  if (scale > 1.0f)
    scale = 1.0f;

  int draw_w = (int)(imgW * scale);
  int draw_h = (int)(imgH * scale);
  if (draw_w < 1)
    draw_w = 1;
  if (draw_h < 1)
    draw_h = 1;

  int start_x = pad + (avail_w - draw_w) / 2;
  int start_y = pad + (avail_h - draw_h) / 2;
  if (start_x < 0)
    start_x = 0;
  if (start_y < 0)
    start_y = 0;

  InlineImageCacheEntry entry;
  entry.image_id = image_id;
  entry.screen_h = (u16)screen_h;
  entry.start_x = (u16)start_x;
  entry.start_y = (u16)start_y;
  entry.width = (u16)draw_w;
  entry.height = (u16)draw_h;
  entry.pixels.resize(draw_w * draw_h);

  u16 bg565 = ts->GetBgColor();
  u8 bg_r5 = (bg565 >> 11) & 0x1F;
  u8 bg_g6 = (bg565 >> 5) & 0x3F;
  u8 bg_b5 = bg565 & 0x1F;
  u8 bg_r8 = (bg_r5 << 3) | (bg_r5 >> 2);
  u8 bg_g8 = (bg_g6 << 2) | (bg_g6 >> 4);
  u8 bg_b8 = (bg_b5 << 3) | (bg_b5 >> 2);

  for (int y = 0; y < draw_h; y++) {
    int src_y = (int)(y / scale);
    if (src_y >= imgH)
      src_y = imgH - 1;
    for (int x = 0; x < draw_w; x++) {
      int src_x = (int)(x / scale);
      if (src_x >= imgW)
        src_x = imgW - 1;
      unsigned char *px = &pixels[(src_y * imgW + src_x) * 4];

      u8 r8 = px[0];
      u8 g8 = px[1];
      u8 b8 = px[2];
      u8 a8 = px[3];
      if (a8 < 255) {
        r8 = (u8)((r8 * a8 + bg_r8 * (255 - a8) + 127) / 255);
        g8 = (u8)((g8 * a8 + bg_g8 * (255 - a8) + 127) / 255);
        b8 = (u8)((b8 * a8 + bg_b8 * (255 - a8) + 127) / 255);
      }

      u16 r = (r8 >> 3) & 0x1F;
      u16 g = (g8 >> 2) & 0x3F;
      u16 b = (b8 >> 3) & 0x1F;
      entry.pixels[(y * draw_w) + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);

  const size_t entry_bytes = entry.pixels.size() * sizeof(u16);
  if (entry_bytes <= xml::book::kInlineImageCacheMaxBytes) {
    while (!inline_image_cache.empty() &&
           inline_image_cache_bytes + entry_bytes >
               xml::book::kInlineImageCacheMaxBytes) {
      inline_image_cache_bytes -=
          inline_image_cache.back().pixels.size() * sizeof(u16);
      inline_image_cache.pop_back();
    }
    inline_image_cache.push_front(std::move(entry));
    inline_image_cache_bytes += entry_bytes;
    blit_cached(inline_image_cache.front());
  } else {
    // Should never happen with 240x400 cap, but keep a direct fallback path.
    blit_cached(entry);
  }

  return true;
}

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
  inline_images.clear();
  ClearInlineImageCache();
  // pages.erase(pages.begin(), pages.end());
}
