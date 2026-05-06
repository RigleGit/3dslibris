/*
    3dslibris - book_io.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Input/output and parser dispatch for non-EPUB formats.
    - UTF-8 normalization and encoding repair utilities.
    - TXT/RTF/ODT loading, extraction, and chapter/index helper generation.
*/

#include "book/book.h"
#include "book/book_parser.h"

u8 Book::Parse(bool fulltext) {
  // Transitional wrapper: kept during Book parser/renderer split.
  return book_parser::Parse(this, fulltext);
}
