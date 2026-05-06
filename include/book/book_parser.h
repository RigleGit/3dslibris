#pragma once

#include <stdint.h>

class Book;

namespace book_parser {

// Transitional parser boundary: kept during Book parser/renderer split.
//
// These functions centralize format dispatch while preserving Book's public
// wrappers and the existing parser implementations.
uint8_t OpenPrepared(Book *book);
uint8_t Parse(Book *book, bool fulltext);
uint8_t IndexMetadata(Book *book, const char *path);

} // namespace book_parser
