#pragma once

#include <stdint.h>
#include <string>

class Book;

uint8_t ParseCbzFile(Book *book, const char *path);
uint8_t IndexCbzMetadata(Book *book, const char *path);
int cbz_extract_cover(Book *book, const std::string &cbzpath);
