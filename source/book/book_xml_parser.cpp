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

#include "book/book.h"

#include "book/book_context.h"
#include "book/book_xml.h"
#include "book/heading_layout.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_bidi_utils.h"
#include "shared/text_unicode_utils.h"
#include "shared/string_utils.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

namespace {

static const size_t kFb2BinaryMaxChars = 6 * 1024 * 1024;

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

static std::string ToLowerAsciiLocal(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static bool ParsedBufferEndsWithWhitespace(const parsedata_t *p) {
  if (!p || p->buflen == 0)
    return false;
  const u32 c = p->buf[p->buflen - 1];
  return c == ' ' || c == '\n' || c == '\t';
}

static void AppendParsedByte(parsedata_t *p, u32 c) {
  parse_append_page_byte(p, c);
}

static void AppendParsedCodepoints(parsedata_t *p, const char *utf8,
                                   size_t utf8_len) {
  size_t offset = 0;
  while (offset < utf8_len) {
    uint32_t cp = 0;
    size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        utf8 + offset, utf8_len - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    parse_append_page_byte(p, (u32)cp);
    offset += consumed;
  }
}

static void EmitBidiSegment(
    parsedata_t *p,
    const std::vector<text_layout_utils::ShapedGlyph> &run,
    size_t seg_start, size_t seg_end,
    const std::vector<text_bidi_utils::BidiRun> &runs) {
  if (seg_start >= seg_end)
    return;
  std::vector<uint32_t> cps;
  cps.reserve(seg_end - seg_start);
  for (size_t i = seg_start; i < seg_end; i++)
    cps.push_back(run[i].text.codepoint);
  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), runs);
  for (size_t i = 0; i < cps.size(); i++)
    parse_append_page_byte(p, (u32)cps[i]);
}

static bool DetectParagraphRTL(
    const std::vector<text_layout_utils::ShapedGlyph> &run) {
  for (size_t i = 0; i < run.size(); i++) {
    uint32_t cp = run[i].text.codepoint;
    if ((cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A))
      return false;
    if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0x0600 && cp <= 0x06FF) ||
        (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFF))
      return true;
  }
  return false;
}

static void RestoreParsedStyleMarkers(parsedata_t *p) {
  if (!p)
    return;
  if (p->superscript)
    AppendParsedByte(p, TEXT_SUPERSCRIPT_ON);
  if (p->subscript)
    AppendParsedByte(p, TEXT_SUBSCRIPT_ON);
  if (p->strikethrough)
    AppendParsedByte(p, TEXT_STRIKETHROUGH_ON);
  if (p->underline)
    AppendParsedByte(p, TEXT_UNDERLINE_ON);
  if (p->italic)
    AppendParsedByte(p, TEXT_ITALIC_ON);
  if (p->bold)
    AppendParsedByte(p, TEXT_BOLD_ON);
}

static void SyncParsedTextStyle(Text *ts, bool bold, bool italic) {
  if (!ts)
    return;
  if (bold && italic)
    ts->SetStyle(TEXT_STYLE_BOLDITALIC);
  else if (bold)
    ts->SetStyle(TEXT_STYLE_BOLD);
  else if (italic)
    ts->SetStyle(TEXT_STYLE_ITALIC);
  else
    ts->SetStyle(TEXT_STYLE_REGULAR);
}

static bool ContainsAsciiNoCase(const std::string &haystack,
                                const char *needle) {
  if (!needle || !needle[0])
    return false;
  const std::string haystack_lc = ToLowerAsciiLocal(haystack);
  const std::string needle_lc = ToLowerAsciiLocal(needle);
  return haystack_lc.find(needle_lc) != std::string::npos;
}

static bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static void ParseInlineStyleFlags(const char *style, bool *bold_out,
                                  bool *italic_out, bool *underline_out,
                                  bool *strikethrough_out,
                                  bool *superscript_out,
                                  bool *subscript_out) {
  if (!style ||
      (!bold_out && !italic_out && !underline_out && !strikethrough_out &&
       !superscript_out && !subscript_out))
    return;
  const std::string value(style);
  const std::string style_lc = ToLowerAsciiLocal(value);

  if (italic_out) {
    if (ContainsAsciiNoCase(style_lc, "font-style:italic") ||
        ContainsAsciiNoCase(style_lc, "font-style: italic") ||
        ContainsAsciiNoCase(style_lc, "font-style:oblique") ||
        ContainsAsciiNoCase(style_lc, "font-style: oblique") ||
        ContainsAsciiNoCase(style_lc, "font:italic") ||
        ContainsAsciiNoCase(style_lc, "font: italic") ||
        ContainsAsciiNoCase(style_lc, "font:oblique") ||
        ContainsAsciiNoCase(style_lc, "font: oblique")) {
      *italic_out = true;
    }
  }

  if (bold_out) {
    if (ContainsAsciiNoCase(style_lc, "font-weight:bold") ||
        ContainsAsciiNoCase(style_lc, "font-weight: bold") ||
        ContainsAsciiNoCase(style_lc, "font-weight:bolder") ||
        ContainsAsciiNoCase(style_lc, "font-weight: bolder") ||
        ContainsAsciiNoCase(style_lc, "font-weight:600") ||
        ContainsAsciiNoCase(style_lc, "font-weight: 600") ||
        ContainsAsciiNoCase(style_lc, "font-weight:700") ||
        ContainsAsciiNoCase(style_lc, "font-weight: 700") ||
        ContainsAsciiNoCase(style_lc, "font-weight:800") ||
        ContainsAsciiNoCase(style_lc, "font-weight: 800") ||
        ContainsAsciiNoCase(style_lc, "font-weight:900") ||
        ContainsAsciiNoCase(style_lc, "font-weight: 900") ||
        ContainsAsciiNoCase(style_lc, "font:bold") ||
        ContainsAsciiNoCase(style_lc, "font: bold")) {
      *bold_out = true;
    }
  }

  if (underline_out) {
    if (ContainsAsciiNoCase(style_lc, "text-decoration:underline") ||
        ContainsAsciiNoCase(style_lc, "text-decoration: underline") ||
        ContainsAsciiNoCase(style_lc, "text-decoration-line:underline") ||
        ContainsAsciiNoCase(style_lc, "text-decoration-line: underline")) {
      *underline_out = true;
    }
  }

  if (strikethrough_out) {
    if (ContainsAsciiNoCase(style_lc, "text-decoration:line-through") ||
        ContainsAsciiNoCase(style_lc, "text-decoration: line-through") ||
        ContainsAsciiNoCase(style_lc, "text-decoration-line:line-through") ||
        ContainsAsciiNoCase(style_lc, "text-decoration-line: line-through")) {
      *strikethrough_out = true;
    }
  }

  if (superscript_out) {
    if (ContainsAsciiNoCase(style_lc, "vertical-align:super") ||
        ContainsAsciiNoCase(style_lc, "vertical-align: super")) {
      *superscript_out = true;
    }
  }

  if (subscript_out) {
    if (ContainsAsciiNoCase(style_lc, "vertical-align:sub") ||
        ContainsAsciiNoCase(style_lc, "vertical-align: sub")) {
      *subscript_out = true;
    }
  }
}

static void ParseClassStyleFlags(const char *class_name, bool *bold_out,
                                 bool *italic_out, bool *underline_out,
                                 bool *strikethrough_out,
                                 bool *superscript_out,
                                 bool *subscript_out) {
  if (!class_name ||
      (!bold_out && !italic_out && !underline_out && !strikethrough_out &&
       !superscript_out && !subscript_out))
    return;
  const std::string class_lc = ToLowerAsciiLocal(class_name);

  if (italic_out) {
    if (ContainsAsciiNoCase(class_lc, "italic") ||
        ContainsAsciiNoCase(class_lc, "oblique") ||
        ContainsAsciiNoCase(class_lc, "emphasis")) {
      *italic_out = true;
    }
  }

  if (bold_out) {
    if (ContainsAsciiNoCase(class_lc, "bold") ||
        ContainsAsciiNoCase(class_lc, "semibold") ||
        ContainsAsciiNoCase(class_lc, "font-weight")) {
      *bold_out = true;
    }
  }

  if (underline_out) {
    if (ContainsAsciiNoCase(class_lc, "underline") ||
        ContainsAsciiNoCase(class_lc, "underlined")) {
      *underline_out = true;
    }
  }

  if (strikethrough_out) {
    if (ContainsAsciiNoCase(class_lc, "strikethrough") ||
        ContainsAsciiNoCase(class_lc, "line-through") ||
        ContainsAsciiNoCase(class_lc, "strike") ||
        ContainsAsciiNoCase(class_lc, "deleted")) {
      *strikethrough_out = true;
    }
  }

  if (superscript_out) {
    if (ContainsAsciiNoCase(class_lc, "superscript")) {
      *superscript_out = true;
    }
  }

  if (subscript_out) {
    if (ContainsAsciiNoCase(class_lc, "subscript")) {
      *subscript_out = true;
    }
  }
}

static void ParseElementStyleFlags(const char **attr, bool *bold_out,
                                   bool *italic_out, bool *underline_out,
                                   bool *strikethrough_out,
                                   bool *superscript_out,
                                   bool *subscript_out) {
  if ((!bold_out && !italic_out && !underline_out && !strikethrough_out &&
       !superscript_out && !subscript_out) ||
      !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "style")) {
      ParseInlineStyleFlags(attr[i + 1], bold_out, italic_out, underline_out,
                            strikethrough_out, superscript_out, subscript_out);
    } else if (AttrNameEquals(attr[i], "class")) {
      ParseClassStyleFlags(attr[i + 1], bold_out, italic_out, underline_out,
                           strikethrough_out, superscript_out, subscript_out);
    }
  }
}

static bool HasClassTokenNoCase(const char *class_name, const char *token) {
  if (!class_name || !token || !token[0])
    return false;
  const std::string class_lc = ToLowerAsciiLocal(class_name);
  const std::string token_lc = ToLowerAsciiLocal(token);
  return ContainsToken(class_lc, token_lc);
}

static void ParseInlineHiddenFlags(const char *style, bool *hidden_out) {
  if (!style || !hidden_out)
    return;
  const std::string style_lc = ToLowerAsciiLocal(style);

  if (ContainsAsciiNoCase(style_lc, "display:none") ||
      ContainsAsciiNoCase(style_lc, "display: none") ||
      ContainsAsciiNoCase(style_lc, "visibility:hidden") ||
      ContainsAsciiNoCase(style_lc, "visibility: hidden") ||
      ContainsAsciiNoCase(style_lc, "clip:rect(0,0,0,0)") ||
      ContainsAsciiNoCase(style_lc, "clip: rect(0, 0, 0, 0)") ||
      ContainsAsciiNoCase(style_lc, "clip-path:inset(50%)") ||
      ContainsAsciiNoCase(style_lc, "clip-path: inset(50%)")) {
    *hidden_out = true;
    return;
  }

  const bool tiny =
      (ContainsAsciiNoCase(style_lc, "width:1px") ||
       ContainsAsciiNoCase(style_lc, "width: 1px")) &&
      (ContainsAsciiNoCase(style_lc, "height:1px") ||
       ContainsAsciiNoCase(style_lc, "height: 1px"));
  const bool offscreen =
      ContainsAsciiNoCase(style_lc, "position:absolute") ||
      ContainsAsciiNoCase(style_lc, "position: absolute");
  const bool hidden_overflow =
      ContainsAsciiNoCase(style_lc, "overflow:hidden") ||
      ContainsAsciiNoCase(style_lc, "overflow: hidden");
  if (tiny && offscreen && hidden_overflow)
    *hidden_out = true;
}

static void ParseClassHiddenFlags(const char *class_name, bool *hidden_out) {
  if (!class_name || !hidden_out)
    return;
  if (HasClassTokenNoCase(class_name, "visually-hidden") ||
      HasClassTokenNoCase(class_name, "visuallyhidden") ||
      HasClassTokenNoCase(class_name, "sr-only") ||
      HasClassTokenNoCase(class_name, "screen-reader-text")) {
    *hidden_out = true;
  }
}

static bool AttrTruthyNoCase(const char *value) {
  if (!value || !value[0])
    return true;
  return EqualsAsciiNoCase(value, "1") || EqualsAsciiNoCase(value, "true") ||
         EqualsAsciiNoCase(value, "yes") ||
         EqualsAsciiNoCase(value, "hidden");
}

static void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  if (!hidden_out || !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (AttrNameEquals(name, "hidden")) {
      *hidden_out = true;
    } else if (AttrNameEquals(name, "aria-hidden")) {
      if (AttrTruthyNoCase(value))
        *hidden_out = true;
    } else if (value && value[0] && AttrNameEquals(name, "style")) {
      ParseInlineHiddenFlags(value, hidden_out);
    } else if (value && value[0] && AttrNameEquals(name, "class")) {
      ParseClassHiddenFlags(value, hidden_out);
    }
  }
}

static bool HasActiveStackBoldStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_bold_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_hidden_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackItalicStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_italic_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_underline_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_strikethrough_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_superscript_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_subscript_stack[i])
      return true;
  }
  return false;
}

static void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight) {
  if (!p || !p->book || !p->ts)
    return;

  Text *ts = p->ts;
  int maxHeight = (p->screen == 1) ? 320 : 400;
  int bottomMargin =
      (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
  if ((p->pen.y + lineheight) <= (maxHeight - bottomMargin))
    return;

  p->perf_page_overflows++;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    page->start = p->pos;
    p->pos += p->buflen;
    page->end = p->pos;
    p->pagecount++;

    parse_reset_page_buffer(p);
    RestoreParsedStyleMarkers(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + lineheight;
}

static int MeasureParsedTextAdvance(uint32_t codepoint, void *ctx) {
  Text *ts = (Text *)ctx;
  return ts ? ts->GetAdvance(codepoint) : 0;
}

struct ChardataPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit ChardataPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~ChardataPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_chardata_calls++;
    parsedata->perf_chardata_ms += (u64)(osGetTime() - t_begin);
  }
};

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

static void linefeed(parsedata_t *p) {
  AppendParsedByte(p, '\n');
  p->pen.x = MARGINLEFT;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

static bool blankline(parsedata_t *p) {
  // Was the preceding text a blank line?
  if (p->buflen < 3)
    return true;
  return (p->buf[p->buflen - 1] == '\n') && (p->buf[p->buflen - 2] == '\n');
}

static void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  Text *ts = p->ts;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    RestoreParsedStyleMarkers(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
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

void instruction(void *data, const char *target, const char *pidata) {}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

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
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    req.screen_height = (p->screen == 1) ? 320 : 400;
    req.bottom_margin =
        (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
    req.line_height = ts->GetHeight();
    req.linespacing = ts->linespacing;
    req.heading_level = 1;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
    parse_push(p, TAG_H1);
    bool lf = !blankline(p);
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    if (lf)
      linefeed(p);
  } else if (!strcmp(el, "h2")) {
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    req.screen_height = (p->screen == 1) ? 320 : 400;
    req.bottom_margin =
        (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
    req.line_height = ts->GetHeight();
    req.linespacing = ts->linespacing;
    req.heading_level = 2;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
    parse_push(p, TAG_H2);
    bool lf = !blankline(p);
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    if (lf)
      linefeed(p);
  } else if (!strcmp(el, "h3")) {
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    req.screen_height = (p->screen == 1) ? 320 : 400;
    req.bottom_margin =
        (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
    req.line_height = ts->GetHeight();
    req.linespacing = ts->linespacing;
    req.heading_level = 3;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
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
    p->in_paragraph = true;
    p->paragraph_has_content = false;
    if (!blankline(p)) {
      for (int i = 0; i < p->book->GetParagraphSpacing(); i++) {
        linefeed(p);
      }
      for (int i = 0; i < p->book->GetParagraphIndent(); i++) {
        AppendParsedByte(p, ' ');
        p->pen.x += ts->GetAdvance(' ');
      }
    }
  } else if (!strcmp(el, "pre")) {
    parse_push(p, TAG_PRE);
    p->preformatted_wrap_enabled = true;
  }
  else if (!strcmp(el, "li")) {
    parse_push(p, TAG_UNKNOWN);
    if (parse_in(p, TAG_UL) && !parse_in(p, TAG_OL)) {
      if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
        linefeed(p);
      AppendParsedByte(p, 0x2022); // bullet '•'
      AppendParsedByte(p, ' ');
      p->pen.x += ts->GetAdvance(0x2022) + ts->GetAdvance(' ');
      p->linebegan = true;
      p->strip_leading_list_marker = true;
    }
  }
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
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    SyncParsedTextStyle(ts, p->bold, p->italic);
  } else if (!strcmp(el, "em") || !strcmp(el, "i")) {
    parse_push(p, TAG_EM);
    AppendParsedByte(p, TEXT_ITALIC_ON);
    p->italic = true;
    SyncParsedTextStyle(ts, p->bold, p->italic);
  } else if (!strcmp(el, "u") || !strcmp(el, "ins")) {
    parse_push(p, TAG_UNDERLINE);
    if (!p->underline) {
      AppendParsedByte(p, TEXT_UNDERLINE_ON);
      p->underline = true;
    }
  } else if (!strcmp(el, "strike") || !strcmp(el, "s") ||
             !strcmp(el, "del")) {
    parse_push(p, TAG_STRIKETHROUGH);
    if (!p->strikethrough) {
      AppendParsedByte(p, TEXT_STRIKETHROUGH_ON);
      p->strikethrough = true;
    }
  } else if (!strcmp(el, "sup")) {
    parse_push(p, TAG_SUPERSCRIPT);
    if (!p->superscript) {
      AppendParsedByte(p, TEXT_SUPERSCRIPT_ON);
      p->superscript = true;
    }
  } else if (!strcmp(el, "sub")) {
    parse_push(p, TAG_SUBSCRIPT);
    if (!p->subscript) {
      AppendParsedByte(p, TEXT_SUBSCRIPT_ON);
      p->subscript = true;
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
      InlineImageLayoutPlan image_plan{};
      const bool leading_paragraph_image =
          p->in_paragraph && !p->paragraph_has_content;
      const InlineImageContext image_context =
          leading_paragraph_image ? INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH
                                  : INLINE_IMAGE_CONTEXT_DEFAULT;
      p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x,
                                     p->pen.y, p->linebegan,
                                     image_context, &image_plan);

      // Mirror the renderer so pagination and draw agree on where the image
      // starts and how much space it consumes.
      if (image_plan.advance_before)
        AdvanceParsedScreen(p);
      if (image_plan.line_break_before && p->linebegan)
        linefeed(p);

      // The token stays format-agnostic; parser and renderer now derive the
      // concrete inline/band/page behavior from the same layout planner.
      if (leading_paragraph_image)
        AppendParsedByte(p, TEXT_IMAGE_LEADING_PARAGRAPH);
      AppendParsedByte(p, TEXT_IMAGE);
      AppendParsedByte(p, (u32)image_id);

      switch (image_plan.mode) {
      case INLINE_IMAGE_LAYOUT_INLINE:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        p->pen.x += image_plan.draw_width + ts->GetAdvance(' ');
        p->linebegan = true;
        break;

      case INLINE_IMAGE_LAYOUT_BAND:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        // Band images are block-level: following text resumes below, while
        // consecutive images may stack only if there is no text between them.
        p->pen.x = ts->margin.left;
        p->pen.y += image_plan.vertical_space_after_draw;
        p->linebegan = false;
        break;

      case INLINE_IMAGE_LAYOUT_PAGE:
      default:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        if (p->screen == 1) {
          AdvanceParsedScreen(p);
        } else {
          p->screen = 1;
          p->pen.x = ts->margin.left;
          p->pen.y = ts->margin.top + ts->GetHeight();
          p->linebegan = false;
        }
        break;
      }
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

  // CSS-based emphasis fallback for EPUBs that do not use semantic tags.
  if (parse_in(p, TAG_BODY) && p->stacksize > 0) {
    bool style_bold = false;
    bool style_italic = false;
    bool style_underline = false;
    bool style_strikethrough = false;
    bool style_superscript = false;
    bool style_subscript = false;
    bool style_hidden = false;
    ParseElementStyleFlags(attr, &style_bold, &style_italic, &style_underline,
                           &style_strikethrough, &style_superscript,
                           &style_subscript);
    ParseElementHiddenFlags(attr, &style_hidden);

    const u8 current = (u8)(p->stacksize - 1);
    p->style_bold_stack[current] = style_bold;
    p->style_italic_stack[current] = style_italic;
    p->style_underline_stack[current] = style_underline;
    p->style_strikethrough_stack[current] = style_strikethrough;
    p->style_superscript_stack[current] = style_superscript;
    p->style_subscript_stack[current] = style_subscript;
    p->style_hidden_stack[current] = style_hidden;

    bool style_changed = false;
    if (style_bold && !p->bold) {
      AppendParsedByte(p, TEXT_BOLD_ON);
      p->pos++;
      p->bold = true;
      style_changed = true;
    }
    if (style_italic && !p->italic) {
      AppendParsedByte(p, TEXT_ITALIC_ON);
      p->italic = true;
      style_changed = true;
    }
    if (style_underline && !p->underline) {
      AppendParsedByte(p, TEXT_UNDERLINE_ON);
      p->underline = true;
      style_changed = true;
    }
    if (style_strikethrough && !p->strikethrough) {
      AppendParsedByte(p, TEXT_STRIKETHROUGH_ON);
      p->strikethrough = true;
      style_changed = true;
    }
    if (style_superscript && !p->superscript) {
      AppendParsedByte(p, TEXT_SUPERSCRIPT_ON);
      p->superscript = true;
      style_changed = true;
    }
    if (style_subscript && !p->subscript) {
      AppendParsedByte(p, TEXT_SUBSCRIPT_ON);
      p->subscript = true;
      style_changed = true;
    }
    if (style_changed)
      SyncParsedTextStyle(ts, p->bold, p->italic);
  }
}

void chardata(void *data, const XML_Char *txt, int txtlen) {
  //! reflow text on the fly, into page data structure.

  parsedata_t *p = (parsedata_t *)data;
  ChardataPerfScope perf_scope(p);
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
  if (HasActiveStackHiddenStyle(p))
    return;
  if ((parse_in(p, TAG_H1) || parse_in(p, TAG_H2) || parse_in(p, TAG_H3)) &&
      p->doc_heading.size() < 160) {
    p->doc_heading.append((const char *)txt, txtlen);
  }

  if (p->strip_leading_list_marker) {
    bool all_whitespace_only = false;
    size_t strip = text_unicode_utils::StripLeadingListMarkerUtf8(
        txt, (size_t)txtlen, &all_whitespace_only);
    if (strip > 0) {
      txt += strip;
      txtlen -= (int)strip;
      p->strip_leading_list_marker = false;
      if (txtlen <= 0)
        return;
    } else if (all_whitespace_only) {
      return;
    } else {
      p->strip_leading_list_marker = false;
    }
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

  if (parse_in(p, TAG_PRE)) {
    if (!p->preformatted_wrap_enabled) {
      int i = 0;
      while (i < txtlen) {
        if (txt[i] == '\r') {
          i++;
          continue;
        }

        if (iswhitespace((u32)(u8)txt[i])) {
          if (txt[i] == '\n') {
            AppendParsedByte(p, '\n');
            p->pen.x = ts->margin.left;
            p->pen.y += (lineheight + linespacing);
            p->linebegan = false;
            AdvanceParsedPageOnOverflow(p, lineheight);
          } else if (p->linebegan && p->buflen &&
                     !iswhitespace(p->buf[p->buflen - 1])) {
            AppendParsedByte(p, ' ');
            p->pen.x += spaceadvance;
          }
          i++;
          continue;
        }

        int j = i;
        int advance = 0;
        u8 bytes = 1;
        for (j = i; (j < txtlen) && (!iswhitespace((u32)(u8)txt[j])); j += bytes) {
          u32 code = (u8)txt[j];
          bytes = 1;
          if (code >> 7)
            bytes =
                ts->GetCharCode((char *)&(txt[j]), (size_t)(txtlen - j), &code);

          advance += ts->GetAdvance(code);
          if (advance >
              ts->display.width - ts->margin.right - ts->margin.left) {
            break;
          }
        }

        if ((p->pen.x + advance) > (ts->display.width - ts->margin.right)) {
          AppendParsedByte(p, '\n');
          p->pen.x = ts->margin.left;
          p->pen.y += (lineheight + linespacing);
          p->linebegan = false;
        }

        AdvanceParsedPageOnOverflow(p, lineheight);

        AppendParsedCodepoints(p, txt + i, (size_t)(j - i));
        p->linebegan = true;
        i = j;
        p->pen.x += advance;
      }
      return;
    }

    std::vector<text_layout_utils::ShapedGlyph> pre_run;
    bool pre_has_rtl = false;
    if (!text_layout_utils::ShapeTextRunBidi(txt, (size_t)txtlen, NULL,
                                             MeasureParsedTextAdvance, ts,
                                             &pre_run, &pre_has_rtl)) {
      return;
    }

    std::vector<text_bidi_utils::BidiRun> pre_bidi_runs;
    if (pre_has_rtl) {
      std::vector<uint32_t> pre_cps;
      pre_cps.reserve(pre_run.size());
      for (size_t ci = 0; ci < pre_run.size(); ci++)
        pre_cps.push_back(pre_run[ci].text.codepoint);
      text_bidi_utils::AnalyzeBidiRuns(pre_cps.data(), pre_cps.size(),
                                       &pre_bidi_runs);
      if (DetectParagraphRTL(pre_run))
        AppendParsedByte(p, TEXT_PARAGRAPH_RTL);
      else
        AppendParsedByte(p, TEXT_PARAGRAPH_LTR);
    }

    const int maxPreLineWidth =
        ts->display.width - ts->margin.right - ts->margin.left;
    size_t unit_index = 0;
    while (unit_index < pre_run.size()) {
      const text_layout_utils::ShapedGlyph &unit = pre_run[unit_index];
      if (unit.text.codepoint == '\r') {
        unit_index++;
        continue;
      }

      if (unit.text.codepoint == '\n') {
        AppendParsedByte(p, '\n');
        p->pen.x = ts->margin.left;
        p->pen.y += (lineheight + linespacing);
        p->linebegan = false;
        AdvanceParsedPageOnOverflow(p, lineheight);
        unit_index++;
        continue;
      }

      text_layout_utils::LineBreakMeasureResult segment =
          text_layout_utils::FindPreformattedLineBreakAndMeasure(
              pre_run, unit_index, maxPreLineWidth);
      size_t segment_end_index = segment.end_index;
      if (segment_end_index <= unit_index)
        segment_end_index = unit_index + 1;

      size_t segment_start = pre_run[unit_index].text.byte_offset;
      size_t segment_end =
          pre_run[segment_end_index - 1].text.byte_offset +
          pre_run[segment_end_index - 1].text.byte_length;
      const int advance = segment.width;

      if ((p->pen.x + advance) > (ts->display.width - ts->margin.right) &&
          p->pen.x > ts->margin.left) {
        AppendParsedByte(p, '\n');
        p->pen.x = ts->margin.left;
        p->pen.y += (lineheight + linespacing);
        p->linebegan = false;
        AdvanceParsedPageOnOverflow(p, lineheight);
      }

      if (pre_has_rtl)
        EmitBidiSegment(p, pre_run, unit_index, segment_end_index,
                        pre_bidi_runs);
      else
        AppendParsedCodepoints(p, txt + segment_start,
                               segment_end - segment_start);
      p->pen.x += advance;
      p->linebegan = true;
      unit_index = segment_end_index;

      if (unit_index < pre_run.size() &&
          pre_run[unit_index].text.codepoint != '\n') {
        AppendParsedByte(p, '\n');
        p->pen.x = ts->margin.left;
        p->pen.y += (lineheight + linespacing);
        p->linebegan = false;
        AdvanceParsedPageOnOverflow(p, lineheight);
      }
    }
    return;
  }

  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(txt, (size_t)txtlen, NULL,
                                           MeasureParsedTextAdvance, ts,
                                           &run, &has_rtl))
    return;

  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  if (has_rtl) {
    std::vector<uint32_t> run_cps;
    run_cps.reserve(run.size());
    for (size_t ci = 0; ci < run.size(); ci++)
      run_cps.push_back(run[ci].text.codepoint);
    text_bidi_utils::AnalyzeBidiRuns(run_cps.data(), run_cps.size(),
                                     &bidi_runs);
    if (DetectParagraphRTL(run))
      AppendParsedByte(p, TEXT_PARAGRAPH_RTL);
    else
      AppendParsedByte(p, TEXT_PARAGRAPH_LTR);
  }

  const int maxLineWidth = ts->display.width - ts->margin.right - ts->margin.left;
  size_t unit_index = 0;
  while (unit_index < run.size()) {
    const text_layout_utils::ShapedGlyph &unit = run[unit_index];
    if (unit.text.codepoint == '\r') {
      unit_index++;
      continue;
    }

    if (unit.text.whitespace) {
      if (unit.text.breakable_space && p->linebegan &&
          !ParsedBufferEndsWithWhitespace(p)) {
        AppendParsedByte(p, ' ');
        p->pen.x += spaceadvance;
      } else if (!unit.text.breakable_space) {
        u16 unit_advance = (u16)unit.advance;
        if ((p->pen.x + unit_advance) > (ts->display.width - ts->margin.right)) {
          AppendParsedByte(p, '\n');
          p->pen.x = ts->margin.left;
          p->pen.y += (lineheight + linespacing);
          p->linebegan = false;
        }
        AdvanceParsedPageOnOverflow(p, lineheight);
        AppendParsedCodepoints(p, txt + unit.text.byte_offset, unit.text.byte_length);
        p->pen.x += unit_advance;
        p->linebegan = true;
      }
      unit_index++;
      continue;
    }

    if (p->in_paragraph)
      p->paragraph_has_content = true;

    size_t segment_start = unit.text.byte_offset;
    text_layout_utils::LineBreakMeasureResult segment =
        text_layout_utils::FindLineBreakAndMeasure(run, unit_index,
                                                   maxLineWidth);
    size_t segment_end_index = segment.end_index;
    if (segment_end_index <= unit_index)
      segment_end_index = unit_index + 1;
    u16 advance = (u16)segment.width;
    size_t segment_end = run[segment_end_index - 1].text.byte_offset +
                         run[segment_end_index - 1].text.byte_length;

    if ((p->pen.x + advance) > (ts->display.width - ts->margin.right)) {
      AppendParsedByte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
    }

    AdvanceParsedPageOnOverflow(p, lineheight);
    if (has_rtl)
      EmitBidiSegment(p, run, unit_index, segment_end_index, bidi_runs);
    else
      AppendParsedCodepoints(p, txt + segment_start,
                             segment_end - segment_start);
    p->linebegan = true;
    p->pen.x += advance;
    unit_index = segment_end_index;
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
        bool finishing_capture =
            (p->fb2_title_depth == 1 && p->fb2_title_capture_depth > 0 &&
             p->fb2_title_capture_depth == p->fb2_section_depth);
        if (finishing_capture && p->book) {
          std::string chapter_title =
              NormalizeFb2ChapterTitle(p->fb2_title_text);
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
    parse_reset_page_buffer(p);
    // Retain styles across the page.
    RestoreParsedStyleMarkers(p);
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
    if (p->paragraph_has_content) {
      linefeed(p);
      linefeed(p);
    }
    p->in_paragraph = false;
    p->paragraph_has_content = false;
  } else if (!strcmp(el, "div")) {
  } else if (!strcmp(el, "h1")) {
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "h2")) {
    linefeed(p);
  } else if (!strcmp(el, "h3") || !strcmp(el, "h4") || !strcmp(el, "h5") ||
             !strcmp(el, "h6") || !strcmp(el, "hr") || !strcmp(el, "pre")) {
    if (!strcmp(el, "pre"))
      p->preformatted_wrap_enabled = false;
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "li") || !strcmp(el, "ul") || !strcmp(el, "ol")) {
    if (!strcmp(el, "li"))
      p->strip_leading_list_marker = false;
    linefeed(p);
  }

  parse_pop(p);

  const bool want_bold = parse_in(p, TAG_STRONG) || parse_in(p, TAG_H1) ||
                         parse_in(p, TAG_H2) || HasActiveStackBoldStyle(p);
  const bool want_italic =
      parse_in(p, TAG_EM) || HasActiveStackItalicStyle(p);
  const bool want_underline =
      parse_in(p, TAG_UNDERLINE) || HasActiveStackUnderlineStyle(p);
  const bool want_strikethrough = parse_in(p, TAG_STRIKETHROUGH) ||
                                  HasActiveStackStrikethroughStyle(p);
  const bool want_superscript = parse_in(p, TAG_SUPERSCRIPT) ||
                                HasActiveStackSuperscriptStyle(p);
  const bool want_subscript =
      parse_in(p, TAG_SUBSCRIPT) || HasActiveStackSubscriptStyle(p);

  bool style_changed = false;
  if (p->bold != want_bold) {
    AppendParsedByte(p, want_bold ? TEXT_BOLD_ON : TEXT_BOLD_OFF);
    if (want_bold)
      p->pos++;
    p->bold = want_bold;
    style_changed = true;
  }
  if (p->italic != want_italic) {
    AppendParsedByte(p, want_italic ? TEXT_ITALIC_ON : TEXT_ITALIC_OFF);
    p->italic = want_italic;
    style_changed = true;
  }
  if (p->underline != want_underline) {
    AppendParsedByte(p, want_underline ? TEXT_UNDERLINE_ON
                                       : TEXT_UNDERLINE_OFF);
    p->underline = want_underline;
    style_changed = true;
  }
  if (p->strikethrough != want_strikethrough) {
    AppendParsedByte(p, want_strikethrough ? TEXT_STRIKETHROUGH_ON
                                           : TEXT_STRIKETHROUGH_OFF);
    p->strikethrough = want_strikethrough;
    style_changed = true;
  }
  if (p->superscript != want_superscript) {
    AppendParsedByte(p, want_superscript ? TEXT_SUPERSCRIPT_ON
                                         : TEXT_SUPERSCRIPT_OFF);
    p->superscript = want_superscript;
    style_changed = true;
  }
  if (p->subscript != want_subscript) {
    AppendParsedByte(p, want_subscript ? TEXT_SUBSCRIPT_ON
                                       : TEXT_SUBSCRIPT_OFF);
    p->subscript = want_subscript;
    style_changed = true;
  }
  if (style_changed)
    SyncParsedTextStyle(ts, p->bold, p->italic);

  int maxHeight = (p->screen == 1) ? 320 : 400;
  int bottomMargin =
      (p->screen == 1) ? MIN(ts->margin.bottom, 16) : ts->margin.bottom;
  int lineheight = ts->GetHeight();
  if ((p->pen.y + lineheight) > (maxHeight - bottomMargin)) {
    if (p->screen == 1) {
      // End of right screen; end of page.
      // Copy in buffered char data into a new page.
      Page *page = p->book->AppendPage();
      page->SetBuffer(p->buf, p->buflen);
      parse_reset_page_buffer(p);
      RestoreParsedStyleMarkers(p);
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
  parsedata_t *p = (parsedata_t *)data;
  int advancespace = p->ts->GetAdvance(' ');
  if (s[0] == '&') {
    int code = 0;
    sscanf(s, "&#%d;", &code);
    if (code) {
      AppendParsedByte(p, (u32)code);
      p->pen.x += p->ts->GetAdvance(code);
      return;
    }

    if (!strncmp(s, "&nbsp;", 5)) {
      AppendParsedByte(p, ' ');
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&quot;")) {
      AppendParsedByte(p, '"');
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&amp;")) {
      AppendParsedByte(p, '&');
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&lt;")) {
      AppendParsedByte(p, '<');
      p->pen.x += advancespace;
      return;
    }
    if (!strcmp(s, "&gt;")) {
      AppendParsedByte(p, '>');
      p->pen.x += advancespace;
      return;
    }
  }
}

} // namespace xml::book
