#include "book/book_parser.h"

#include "book/book.h"
#include "book/book_parse_deps.h"
#include "formats/cbz/cbz.h"
#include "formats/common/book_error.h"
#include "formats/common/plain_parser.h"
#include "formats/common/xml_book_parser.h"
#include "formats/epub/epub.h"
#include "formats/mobi/mobi_book_hooks.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/odt/odt_loader.h"
#include "formats/pdf/pdf.h"
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
  return plain_parser::ParseTxtFile(book, path);
}

bool CanParseRtf(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".rtf");
}

uint8_t ParseRtf(Book *book, const char *path, bool) {
  return plain_parser::ParseRtfFile(book, path);
}

bool CanParseOdt(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".odt");
}

uint8_t ParseOdt(Book *book, const char *path, bool) {
  return odt_loader::ParseOdtFile(book, path);
}

bool CanParseMobi(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".mobi");
}

const mobi_parser::Hooks &SharedMobiHooks() {
  static const mobi_parser::Hooks hooks = mobi_book_hooks::Make();
  return hooks;
}

uint8_t ParseMobi(Book *book, const char *path, bool) {
  return mobi_parser::ParseFile(book, path, SharedMobiHooks());
}

bool CanParsePdf(Book *book, bool fulltext) {
  return fulltext &&
         (IsExt(book, ".pdf") || IsExt(book, ".xps") ||
          IsExt(book, ".oxps"));
}

uint8_t ParsePdf(Book *book, const char *path, bool) {
  return ParsePdfFile(book, path);
}

bool CanParseCbz(Book *book, bool fulltext) {
  return fulltext && IsExt(book, ".cbz");
}

uint8_t ParseCbz(Book *book, const char *path, bool) {
  return ParseCbzFile(book, path);
}

static const BookParserEntry kBookParsers[] = {
    {"txt", CanParseTxt, ParseTxt},
    {"rtf", CanParseRtf, ParseRtf},
    {"odt", CanParseOdt, ParseOdt},
    {"mobi", CanParseMobi, ParseMobi},
    {"pdf", CanParsePdf, ParsePdf},
    {"cbz", CanParseCbz, ParseCbz},
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

uint8_t OpenPrepared(Book *book) {
  if (!book)
    return 1;

  const std::string path = BuildPath(book);
  char logmsg[256];
  snprintf(logmsg, sizeof(logmsg), "Opening: %s", path.c_str());
  DBG_LOG(book->GetStatusReporter(), logmsg);

  uint8_t err = 1;
  if (book->format == FORMAT_EPUB)
    err = epub(book, path, false);
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
  return xml_book_parser::ParseXmlBookFile(
      book, path, fulltext, deps, plain_parser::BuildFb2FallbackChapters,
      plain_parser::SetNonEpubTocConfidence);
}

uint8_t IndexMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 1;
  if (book->format == FORMAT_EPUB)
    return (uint8_t)epub(book, path, true);
  if (book->format == FORMAT_PDF)
    return IndexPdfMetadata(book, path);
  if (book->format == FORMAT_CBZ)
    return IndexCbzMetadata(book, path);
  return 0;
}

} // namespace book_parser
