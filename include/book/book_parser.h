#pragma once

#include <stdint.h>

class Book;

namespace book_parser {

uint8_t Open(Book *book);
uint8_t OpenPrepared(Book *book);
uint8_t Parse(Book *book, bool fulltext);
uint8_t Index(Book *book);
uint8_t IndexMetadata(Book *book, const char *path);

} // namespace book_parser
