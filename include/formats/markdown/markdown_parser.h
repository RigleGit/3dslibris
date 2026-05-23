#pragma once

#include <3ds/types.h>

class Book;

namespace markdown_parser {

uint8_t Parse(Book *book, const char *path);

} // namespace markdown_parser
