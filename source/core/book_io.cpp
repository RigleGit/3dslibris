#include "book.h"

#include "epub.h"
#include "main.h"
#include "parse.h"
#include "unzip.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <vector>

namespace {

static const size_t kPlainTextMaxBytes = 12 * 1024 * 1024;
static const size_t kOdtContentMaxBytes = 24 * 1024 * 1024;

static bool HasExtCI(const char *name, const char *ext) {
  if (!name || !ext)
    return false;
  size_t nlen = strlen(name);
  size_t elen = strlen(ext);
  if (elen == 0 || nlen < elen)
    return false;
  return strcasecmp(name + nlen - elen, ext) == 0;
}

static bool LooksLikeValidUtf8Bytes(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    if (i + need >= s.size())
      return false;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80)
        return false;
    }
    i += need + 1;
  }
  return true;
}

static void AppendUtf8Codepoint(std::string *out, u32 cp) {
  if (!out)
    return;
  if (cp <= 0x7F) {
    out->push_back((char)cp);
  } else if (cp <= 0x7FF) {
    out->push_back((char)(0xC0 | (cp >> 6)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

static void AppendCp1252Byte(std::string *out, unsigned char b) {
  static const u16 cp1252_map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  if (!out)
    return;
  if (b < 0x80) {
    out->push_back((char)b);
    return;
  }
  if (b >= 0x80 && b <= 0x9F) {
    u16 mapped = cp1252_map[b - 0x80];
    if (mapped != 0x0000)
      AppendUtf8Codepoint(out, mapped);
    else
      out->push_back('?');
    return;
  }
  AppendUtf8Codepoint(out, b);
}

static std::string DecodeLegacySingleByteToUtf8(const std::string &in) {
  std::string out;
  out.reserve(in.size() * 2);
  for (size_t i = 0; i < in.size(); i++)
    AppendCp1252Byte(&out, (unsigned char)in[i]);
  return out;
}

static std::string NormalizeTextUtf8(const std::string &raw) {
  std::string s = raw;
  if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
      (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
    s.erase(0, 3);
  }
  if (LooksLikeValidUtf8Bytes(s))
    return s;
  return DecodeLegacySingleByteToUtf8(s);
}

static void NormalizeNewlines(std::string *s) {
  if (!s)
    return;
  std::string out;
  out.reserve(s->size());
  for (size_t i = 0; i < s->size(); i++) {
    char c = (*s)[i];
    if (c == '\r') {
      if (i + 1 < s->size() && (*s)[i + 1] == '\n')
        i++;
      out.push_back('\n');
    } else {
      out.push_back(c);
    }
  }
  s->swap(out);
}

static bool ReadFileToStringLimited(const char *path, std::string *out,
                                    size_t max_bytes) {
  if (!path || !out)
    return false;
  out->clear();

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;

  char buf[4096];
  while (true) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n > 0) {
      if (out->size() + n > max_bytes) {
        fclose(fp);
        return false;
      }
      out->append(buf, n);
    }
    if (n < sizeof(buf)) {
      if (ferror(fp)) {
        fclose(fp);
        return false;
      }
      break;
    }
  }

  fclose(fp);
  return true;
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
}

static u8 ParsePlainTextBuffer(Book *book, const std::string &text_utf8) {
  if (!book || !book->GetApp() || !book->GetApp()->ts)
    return 1;

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.app = book->GetApp();
  parsedata.ts = book->GetApp()->ts;
  parsedata.prefs = book->GetApp()->prefs;
  parsedata.book = book;

  parse_push(&parsedata, TAG_PRE);
  if (!text_utf8.empty())
    xml::book::chardata(&parsedata, text_utf8.c_str(), (int)text_utf8.size());
  parse_pop(&parsedata);
  FinalizePlainPage(&parsedata);
  return 0;
}

static int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static std::string DecodeRtfToUtf8(const std::string &rtf) {
  std::string out;
  out.reserve(rtf.size());

  std::vector<bool> skip_stack;
  skip_stack.push_back(false);

  for (size_t i = 0; i < rtf.size();) {
    bool skip = skip_stack.back();
    char c = rtf[i];

    if (c == '{') {
      skip_stack.push_back(skip);
      i++;
      continue;
    }
    if (c == '}') {
      if (skip_stack.size() > 1)
        skip_stack.pop_back();
      i++;
      continue;
    }
    if (c != '\\') {
      if (!skip)
        out.push_back(c);
      i++;
      continue;
    }

    if (i + 1 >= rtf.size()) {
      i++;
      continue;
    }

    char n = rtf[i + 1];
    if (n == '\\' || n == '{' || n == '}') {
      if (!skip)
        out.push_back(n);
      i += 2;
      continue;
    }
    if (n == '*') {
      skip_stack.back() = true;
      i += 2;
      continue;
    }
    if (n == '\'') {
      if (i + 3 < rtf.size()) {
        int h1 = HexDigit(rtf[i + 2]);
        int h2 = HexDigit(rtf[i + 3]);
        if (h1 >= 0 && h2 >= 0 && !skip) {
          unsigned char b = (unsigned char)((h1 << 4) | h2);
          AppendCp1252Byte(&out, b);
        }
        i += 4;
      } else {
        i += 2;
      }
      continue;
    }
    if (n == '~') {
      if (!skip)
        out.push_back(' ');
      i += 2;
      continue;
    }
    if (n == '_') {
      if (!skip)
        out.push_back('-');
      i += 2;
      continue;
    }
    if (n == '-') {
      i += 2;
      continue;
    }
    if (n == 'u') {
      size_t p = i + 2;
      int sign = 1;
      if (p < rtf.size() && (rtf[p] == '-' || rtf[p] == '+')) {
        if (rtf[p] == '-')
          sign = -1;
        p++;
      }
      int value = 0;
      bool any = false;
      while (p < rtf.size() && isdigit((unsigned char)rtf[p])) {
        value = value * 10 + (rtf[p] - '0');
        p++;
        any = true;
      }
      if (any && !skip) {
        int cp = sign * value;
        if (cp < 0)
          cp += 65536;
        if (cp >= 0)
          AppendUtf8Codepoint(&out, (u32)cp);
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;
      if (p < rtf.size() && rtf[p] != '\\' && rtf[p] != '{' && rtf[p] != '}')
        p++;
      i = p;
      continue;
    }
    if (isalpha((unsigned char)n)) {
      size_t p = i + 1;
      while (p < rtf.size() && isalpha((unsigned char)rtf[p]))
        p++;
      std::string word = rtf.substr(i + 1, p - (i + 1));
      if (p < rtf.size() && (rtf[p] == '-' || rtf[p] == '+' ||
                             isdigit((unsigned char)rtf[p]))) {
        p++;
        while (p < rtf.size() && isdigit((unsigned char)rtf[p]))
          p++;
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;

      if (!skip) {
        if (word == "par" || word == "line") {
          out.push_back('\n');
        } else if (word == "tab") {
          out.push_back('\t');
        } else if (word == "emdash") {
          AppendUtf8Codepoint(&out, 0x2014);
        } else if (word == "endash") {
          AppendUtf8Codepoint(&out, 0x2013);
        } else if (word == "bullet") {
          AppendUtf8Codepoint(&out, 0x2022);
        } else if (word == "lquote") {
          AppendUtf8Codepoint(&out, 0x2018);
        } else if (word == "rquote") {
          AppendUtf8Codepoint(&out, 0x2019);
        } else if (word == "ldblquote") {
          AppendUtf8Codepoint(&out, 0x201C);
        } else if (word == "rdblquote") {
          AppendUtf8Codepoint(&out, 0x201D);
        }
      }
      i = p;
      continue;
    }

    i += 2;
  }

  return out;
}

static u8 ParseTxtFile(Book *book, const char *path) {
  std::string raw;
  if (!ReadFileToStringLimited(path, &raw, kPlainTextMaxBytes))
    return 252;
  NormalizeNewlines(&raw);
  std::string text = NormalizeTextUtf8(raw);
  return ParsePlainTextBuffer(book, text);
}

static u8 ParseRtfFile(Book *book, const char *path) {
  std::string raw;
  if (!ReadFileToStringLimited(path, &raw, kPlainTextMaxBytes))
    return 252;
  std::string text = DecodeRtfToUtf8(raw);
  NormalizeNewlines(&text);
  if (!LooksLikeValidUtf8Bytes(text))
    text = NormalizeTextUtf8(text);
  return ParsePlainTextBuffer(book, text);
}

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

struct OdtParseState {
  parsedata_t *parsedata;
  int depth;
  int office_text_depth;
  bool pending_space;
};

static bool ParsedataEndsWhitespace(parsedata_t *p) {
  if (!p || p->buflen <= 0)
    return true;
  unsigned char c = p->buf[p->buflen - 1];
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
}

static void odt_chardata(void *userdata, const char *txt, int txtlen) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s || s->office_text_depth <= 0 || !txt || txtlen <= 0)
    return;

  std::string out;
  out.reserve((size_t)txtlen + 1);
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
}

static void odt_end(void *userdata, const char *el) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s)
    return;

  const char *local = XmlLocalName(el);
  if (s->office_text_depth > 0 && OdtIsParagraphTag(local)) {
    OdtEmitParagraphBreak(s);
  }

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

static u8 ParseOdtFile(Book *book, const char *path) {
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

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.app = book->GetApp();
  parsedata.ts = parsedata.app ? parsedata.app->ts : NULL;
  parsedata.prefs = parsedata.app ? parsedata.app->prefs : NULL;
  parsedata.book = book;
  parse_push(&parsedata, TAG_PRE);

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    parse_pop(&parsedata);
    return 254;
  }

  OdtParseState odt_state;
  odt_state.parsedata = &parsedata;
  odt_state.depth = 0;
  odt_state.office_text_depth = 0;
  odt_state.pending_space = false;

  XML_SetUserData(p, &odt_state);
  XML_SetElementHandler(p, odt_start, odt_end);
  XML_SetCharacterDataHandler(p, odt_chardata);

  bool ok = XML_Parse(p, content_xml.c_str(), (int)content_xml.size(), 1) !=
            XML_STATUS_ERROR;
  if (!ok && parsedata.app) {
    parsedata.app->parse_error(p);
  }

  XML_ParserFree(p);
  parse_pop(&parsedata);

  if (!ok)
    return 255;

  FinalizePlainPage(&parsedata);
  return 0;
}

} // namespace

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
  tocResolveTried = false;
  tocResolved = false;
  ClearChapterAnchors();
  u8 err = 1;
  if (format == FORMAT_EPUB) {
    err = epub(this, path, false);
  } else {
    err = Parse(true);
  }
  if (!err)
    if (position > (int)pages.size())
      position = pages.size() - 1;
  return err;
}

u8 Book::Index() {
  if (metadataIndexTried)
    return metadataIndexed ? 0 : 1;
  metadataIndexTried = true;

  int err = 1;
  if (format == FORMAT_EPUB) {
    std::string path;
    path.append(GetFolderName());
    path.append("/");
    path.append(GetFileName());
    err = epub(this, path, true);
  } else {
    // Non-EPUB files currently use filename labels in browser; defer full parse
    // until open to keep startup responsive.
    err = 0;
  }
  if (!err) {
    metadataIndexed = true;
  }
  return err;
}

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char path[MAXPATHLEN];
  snprintf(path, sizeof(path), "%s/%s", GetFolderName(), GetFileName());

  // Lightweight non-XML formats.
  if (fulltext && HasExtCI(GetFileName(), ".txt"))
    return ParseTxtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".rtf"))
    return ParseRtfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".odt"))
    return ParseOdtFile(this, path);

  char *filebuf = new char[BUFSIZE];
  if (!filebuf) {
    rc = 1;
    return (rc);
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    delete[] filebuf;
    rc = 255;
    return (rc);
  }

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.cachefile = fopen("/cache.dat", "w");
  parsedata.app = app;
  parsedata.ts = app ? app->ts : NULL;
  parsedata.prefs = app ? app->prefs : NULL;
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
      app->parse_error(p);
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
