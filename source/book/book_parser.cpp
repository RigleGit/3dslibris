#include "book/book_parser.h"

#include "book/book.h"
#include "book/book_parse_deps.h"
#include "formats/cbz/cbz_parser.h"
#include "formats/common/book_error.h"
#include "formats/common/book_meta_cache.h"
#include "formats/common/xml_book_parser.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/markdown/markdown_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/odt/odt_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "formats/rtf/rtf_parser.h"
#include "formats/txt/txt_parser.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"

#include <stdio.h>
#include <sys/param.h>
#include <sys/stat.h>

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

bool CanParseMarkdown(Book *book, bool fulltext) {
  return fulltext && (IsExt(book, ".md") || IsExt(book, ".markdown"));
}

uint8_t ParseMarkdown(Book *book, const char *path, bool) {
  return markdown_parser::Parse(book, path);
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
    {"markdown", CanParseMarkdown, ParseMarkdown},
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

uint8_t Index(Book *book) {
  if (!book)
    return 1;

  if (book->metadataIndexTried)
    return book->metadataIndexed ? 0 : 1;

  if (book->TryLoadMetadataFromCache())
    return 0;

  std::string path;
  path.append(book->GetFolderName());
  path.append("/");
  path.append(book->GetFileName());

  struct stat st;
  long long fsize = 0, fmtime = 0;
  if (stat(path.c_str(), &st) == 0) {
    fsize  = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }

  book->metadataIndexTried = true;
  int err = IndexMetadata(book, path.c_str());

  if (err == BOOK_ERR_CANCELLED) {
    book->metadataIndexTried = false;
    book->metadataIndexed    = false;
    return (uint8_t)err;
  }

  if (!err) {
    book->metadataIndexed = true;
    book_meta_cache::MetaEntry entry;
    const char *t = book->GetTitle();
    entry.title            = t ? t : "";
    entry.author           = book->GetAuthor();
    entry.cover_image_path = book->coverImagePath;
    book_meta_cache::Save(
        book_meta_cache::BuildPath(path, fsize, fmtime), entry);
  }

  return (uint8_t)err;
}

} // namespace book_parser
