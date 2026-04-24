/*
    3dslibris - odt_loader.cpp

    OpenDocument Text parser extracted from book_io.cpp.
*/

#include "formats/odt/odt_loader.h"

#include <stdio.h>
#include <string.h>

#include "book/book_parse_deps.h"
#include "book/page.h"
#include "book/book_xml.h"
#include "formats/common/xml_parse_utils.h"
#include "minizip/unzip.h"
#include "parse.h"
#include "shared/string_utils.h"

namespace {

static const size_t kOdtContentMaxBytes = 24 * 1024 * 1024;

static const char *XmlLocalName(const char *name) {
  if (!name)
    return "";
  const char *colon = strrchr(name, ':');
  return colon ? (colon + 1) : name;
}

static bool XmlAttrLocalNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle))
    return true;
  const char *local = XmlLocalName(name);
  return !strcmp(local, needle);
}

static bool OdtIsParagraphTag(const char *local_name) {
  if (!local_name)
    return false;
  return !strcmp(local_name, "p") || !strcmp(local_name, "h") ||
         !strcmp(local_name, "list-item") || !strcmp(local_name, "table-row");
}

static bool AddChapterAtPageIfUnique(Book *book, u16 page,
                                     const std::string &title, u8 level) {
  if (!book)
    return false;

  std::string clean = Trim(title);
  if (clean.empty())
    return false;

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    const ChapterEntry &entry = chapters[i];
    if (entry.page == page && entry.level == level && entry.title == clean)
      return false;
  }

  book->AddChapter(page, clean, level);
  return true;
}

static void AddChapterIfUnique(Book *book, const std::string &title, u8 level) {
  if (!book)
    return;
  AddChapterAtPageIfUnique(book, book->GetPageCount(), title, level);
}

static void SetNonEpubTocConfidence(Book *book, bool strong) {
  if (!book)
    return;
  size_t n = book->GetChapters().size();
  if (n == 0) {
    book->ClearTocConfidence();
    return;
  }
  u16 count = (n > 65535) ? 65535 : (u16)n;
  if (strong)
    book->SetTocConfidence(TOC_QUALITY_STRONG, count, 0, 0);
  else
    book->SetTocConfidence(TOC_QUALITY_HEURISTIC, 0, count, 0);
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
}

struct OdtParseState {
  parsedata_t *parsedata;
  Book *book;
  int depth;
  int office_text_depth;
  int heading_depth;
  int heading_level;
  std::string heading_text;
  bool pending_space;
};

static bool ParsedataEndsWhitespace(parsedata_t *p) {
  if (!p || p->buflen <= 0)
    return true;
  u32 c = p->buf[p->buflen - 1];
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static void OdtEmit(OdtParseState *s, const char *txt, int len) {
  if (!s || !s->parsedata || !txt || len <= 0)
    return;
  xml::book::chardata(s->parsedata, txt, len);
}

static void OdtEmitParagraphBreak(OdtParseState *s) {
  if (!s)
    return;
  OdtEmit(s, "\n\n", 2);
  s->pending_space = false;
}

static void odt_start(void *userdata, const char *el, const char **attr) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s)
    return;
  s->depth++;

  if (!strcmp(el, "office:text")) {
    s->office_text_depth = s->depth;
    s->pending_space = false;
    return;
  }

  if (s->office_text_depth <= 0)
    return;

  const char *local = XmlLocalName(el);
  if (!strcmp(local, "line-break")) {
    OdtEmit(s, "\n", 1);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "tab")) {
    OdtEmit(s, "\t", 1);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "s")) {
    int count = 1;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (!XmlAttrLocalNameEquals(attr[i], "c"))
        continue;
      int parsed = atoi(attr[i + 1] ? attr[i + 1] : "1");
      if (parsed > 0 && parsed < 64)
        count = parsed;
      break;
    }
    std::string spaces((size_t)count, ' ');
    OdtEmit(s, spaces.c_str(), (int)spaces.size());
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "image")) {
    OdtEmit(s, "\n[illustration]\n", 15);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "h")) {
    s->heading_depth++;
    s->heading_level = 0;
    s->heading_text.clear();
    for (int i = 0; attr && attr[i]; i += 2) {
      if (!XmlAttrLocalNameEquals(attr[i], "outline-level"))
        continue;
      int level = atoi(attr[i + 1] ? attr[i + 1] : "1");
      if (level < 1)
        level = 1;
      if (level > 7)
        level = 7;
      s->heading_level = level - 1;
      break;
    }
  }
}

static void odt_chardata(void *userdata, const char *txt, int txtlen) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s || s->office_text_depth <= 0 || !txt || txtlen <= 0)
    return;

  static std::string out;
  out.clear();
  for (int i = 0; i < txtlen; i++) {
    unsigned char c = (unsigned char)txt[i];
    bool is_space = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    if (is_space) {
      s->pending_space = true;
      continue;
    }
    if (s->pending_space) {
      if (!ParsedataEndsWhitespace(s->parsedata) || !out.empty())
        out.push_back(' ');
      s->pending_space = false;
    }
    out.push_back((char)c);
  }
  if (!out.empty())
    OdtEmit(s, out.c_str(), (int)out.size());
  if (s->heading_depth > 0 && !out.empty()) {
    if (!s->heading_text.empty() && s->heading_text.back() != ' ')
      s->heading_text.push_back(' ');
    s->heading_text += out;
  }
}

static void odt_end(void *userdata, const char *el) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s)
    return;

  const char *local = XmlLocalName(el);
  if (s->office_text_depth > 0 && !strcmp(local, "h") && s->heading_depth > 0) {
    s->heading_depth--;
    if (s->heading_depth == 0 && s->book) {
      AddChapterIfUnique(s->book, s->heading_text, (u8)s->heading_level);
      s->heading_text.clear();
      s->heading_level = 0;
    }
  }
  if (s->office_text_depth > 0 && OdtIsParagraphTag(local))
    OdtEmitParagraphBreak(s);

  if (s->office_text_depth > 0 && s->depth == s->office_text_depth &&
      !strcmp(el, "office:text")) {
    s->office_text_depth = 0;
    s->pending_space = false;
  }
  s->depth--;
}

static bool ReadZipEntryToStringLimited(unzFile uf, const char *entry_name,
                                        std::string *out, size_t max_bytes) {
  if (!uf || !entry_name || !out)
    return false;
  out->clear();

  if (unzLocateFile(uf, entry_name, 2) != UNZ_OK)
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  char buf[8192];
  int n = 0;
  bool ok = true;
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    if (out->size() + (size_t)n > max_bytes) {
      ok = false;
      break;
    }
    out->append(buf, n);
  }
  if (n < 0)
    ok = false;
  unzCloseCurrentFile(uf);
  return ok;
}

} // namespace

namespace odt_loader {

u8 ParseOdtFile(Book *book, const char *path) {
  if (!book || !path)
    return 251;

  unzFile uf = unzOpen(path);
  if (!uf)
    return 252;

  std::string content_xml;
  bool loaded = ReadZipEntryToStringLimited(uf, "content.xml", &content_xml,
                                            kOdtContentMaxBytes);
  unzClose(uf);
  if (!loaded || content_xml.empty())
    return 253;

  const BookParseDeps deps = BuildBookParseDeps(book);
  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.reporter = deps.reporter;
  parsedata.ts = deps.ts;
  parsedata.prefs = deps.prefs;
  parsedata.book = book;

  parse_push(&parsedata, TAG_PRE);
  book->ClearChapters();
  book->ClearTocConfidence();

  OdtParseState odt_state;
  odt_state.parsedata = &parsedata;
  odt_state.book = book;
  odt_state.depth = 0;
  odt_state.office_text_depth = 0;
  odt_state.heading_depth = 0;
  odt_state.heading_level = 0;
  odt_state.heading_text.clear();
  odt_state.pending_space = false;

  xml_parse_utils::XmlParserOptions options;
  options.start_element = odt_start;
  options.end_element = odt_end;
  options.character_data = odt_chardata;
  options.user_data = &odt_state;
  xml_parse_utils::XmlParseResult parse_result =
      xml_parse_utils::ParseXmlString(content_xml, options);
  bool ok = parse_result.ok;
  if (!ok && parsedata.reporter)
    parsedata.reporter->PrintStatus(
        xml_parse_utils::FormatXmlParseError(parse_result).c_str());
  parse_pop(&parsedata);

  if (!ok)
    return 255;

  FinalizePlainPage(&parsedata);
  SetNonEpubTocConfidence(book, true);
  return 0;
}

} // namespace odt_loader
