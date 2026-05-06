#include "book/book_parser.h"

#include "book/book.h"
#include "book/book_parse_deps.h"
#include "formats/cbz/cbz_parser.h"
#include "formats/common/book_error.h"
#include "formats/common/xml_book_parser.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/odt/odt_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "formats/rtf/rtf_parser.h"
#include "formats/txt/txt_parser.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"

#include <stdio.h>
#include <sys/param.h>

namespace {

typedef bool (*BookParserCanParseFn)(Book *book, bool fulltext);
typedef uint8_t (*BookParserParseFn)(Book *book, const char *path,
                                     bool fulltext);

struct BookParserEntry {
  const char *name;
  BookParserCanParseFn can_parse;
  BookParserParseFn parse;
};

bool IsExt(Book *book, const char *ext) {
  return book && HasExtCI(book->GetFileName(), ext);
}

bool CanParseTxt(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".txt");
}

uint8_t ParseTxt(Book *book, const char *path, bool) {
  return txt_parser::Parse(book, path);
}

bool CanParseRtf(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".rtf");
}

uint8_t ParseRtf(Book *book, const char *path, bool) {
  return rtf_parser::Parse(book, path);
}

bool CanParseOdt(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".odt");
}

uint8_t ParseOdt(Book *book, const char *path, bool) {
  return odt_parser::Parse(book, path);
}

bool CanParseMobi(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".mobi");
}

uint8_t ParseMobi(Book *book, const char *path, bool) {
  return mobi_parser::Parse(book, path);
}

bool CanParsePdf(Book *book, bool fulltext) {
  return fulltext &&
         (IsExt(book, ".pdf") || IsExt(book, ".xps") ||
          IsExt(book, ".oxps"));
}

uint8_t ParsePdf(Book *book, const char *path, bool) {
  return pdf_parser::Parse(book, path);
}

bool CanParseCbz(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".cbz");
}

uint8_t ParseCbz(Book *book, const char *path, bool) {
  return cbz_parser::Parse(book, path);
}

bool CanParseFb2(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".fb2");
}

uint8_t ParseFb2(Book *book, const char *path, bool fulltext) {
  const BookParseDeps deps = BuildBookParseDeps(book);
  return fb2_parser::Parse(book, path, fulltext, deps);
}

static const BookParserEntry kBookParsers[] = {
    {"txt", CanParseTxt, ParseTxt},
    {"rtf", CanParseRtf, ParseRtf},
    {"odt", CanParseOdt, ParseOdt},
    {"mobi", CanParseMobi, ParseMobi},
    {"pdf", CanParsePdf, ParsePdf},
    {"cbz", CanParseCbz, ParseCbz},
    {"fb2", CanParseFb2, ParseFb2},
};

std::string BuildPath(Book *book) {
  std::string path;
  if (!book)
    return path;
  path.append(book->GetFolderName());
  path.append("/");
  path.append(book->GetFileName());
  return path;
}

} // namespace

namespace book_parser {

uint8_t Open(Book *book) {
  if (!book)
    return 1;
  book->PrepareForOpen();
  return OpenPrepared(book);
}

uint8_t OpenPrepared(Book *book) {
  if (!book)
    return 1;

  const std::string path = BuildPath(book);
  char logmsg[256];
  snprintf(logmsg, sizeof(logmsg), "Opening: %s", path.c_str());
  DBG_LOG(book->GetStatusReporter(), logmsg);

  uint8_t err = 1;
  if (book->format == FORMAT_EPUB)
    err = epub_parser::Open(book, path);
  else
    err = Parse(book, true);

  if (!err && book->GetPosition() > (int)book->GetPageCount())
    book->SetPosition((int)book->GetPageCount() - 1);
  return err;
}

uint8_t Parse(Book *book, bool fulltext) {
  if (!book)
    return 1;

  const std::string path_storage = BuildPath(book);
  const char *path = path_storage.c_str();

  for (size_t i = 0; i < sizeof(kBookParsers) / sizeof(kBookParsers[0]); i++) {
    const BookParserEntry &entry = kBookParsers[i];
    if (entry.can_parse(book, fulltext))
      return entry.parse(book, path, fulltext);
  }

  const BookParseDeps deps = BuildBookParseDeps(book);
  return xml_book_parser::ParseXmlBookFile(book, path, fulltext, deps,
                                           nullptr, nullptr);
}

uint8_t IndexMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 1;
  if (book->format == FORMAT_EPUB)
    return epub_parser::Index(book, path);
  if (book->format == FORMAT_PDF)
    return pdf_parser::Index(book, path);
  if (book->format == FORMAT_CBZ)
    return cbz_parser::Index(book, path);
  return 0;
}

} // namespace book_parser
