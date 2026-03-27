// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/cbz/cbz.h"

#include "formats/mupdf/mupdf_document.h"

uint8_t ParseCbzFile(Book *book, const char *path) {
  return ParseMuPdfFile(book, path);
}

uint8_t IndexCbzMetadata(Book *book, const char *path) {
  return IndexMuPdfMetadata(book, path);
}
