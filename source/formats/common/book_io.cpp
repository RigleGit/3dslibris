/*
    3dslibris - book_io.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Input/output and parser dispatch for non-EPUB formats.
    - UTF-8 normalization and encoding repair utilities.
    - TXT/RTF/ODT loading, extraction, and chapter/index helper generation.
*/

#include "book/book.h"
#include "book/book_parse_deps.h"

#include "formats/common/plain_parser.h"
#include "formats/mobi/mobi_book_hooks.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/common/xml_book_parser.h"
#include "formats/pdf/pdf.h"
#include "formats/cbz/cbz.h"
#include "formats/odt/odt_loader.h"
#include "debug_log.h"
#include "shared/string_utils.h"
#include <stdio.h>
#include <sys/param.h>

namespace {

const mobi_parser::Hooks &SharedMobiHooks() {
  static const mobi_parser::Hooks hooks = mobi_book_hooks::Make();
  return hooks;
}

} // namespace

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char path[MAXPATHLEN];
  snprintf(path, sizeof(path), "%s/%s", GetFolderName(), GetFileName());

  // Lightweight non-XML formats.
  if (fulltext && HasExtCI(GetFileName(), ".txt"))
    return plain_parser::ParseTxtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".rtf"))
    return plain_parser::ParseRtfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".odt"))
    return odt_loader::ParseOdtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".mobi")) {
    const mobi_parser::Hooks &hooks = SharedMobiHooks();
    return mobi_parser::ParseFile(this, path, hooks);
  }
  if (fulltext && (HasExtCI(GetFileName(), ".pdf") ||
                   HasExtCI(GetFileName(), ".xps") ||
                   HasExtCI(GetFileName(), ".oxps")))
    return ParsePdfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".cbz"))
    return ParseCbzFile(this, path);

  const BookParseDeps deps = BuildBookParseDeps(this);
  rc = xml_book_parser::ParseXmlBookFile(
      this, path, fulltext, deps, plain_parser::BuildFb2FallbackChapters,
      plain_parser::SetNonEpubTocConfidence);
  return rc;
}

