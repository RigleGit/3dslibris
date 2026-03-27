#pragma once

#include <stdint.h>

class Book;

uint8_t ParseCbzFile(Book *book, const char *path);
uint8_t IndexCbzMetadata(Book *book, const char *path);
