#pragma once

#include <stdint.h>

class Book;

uint8_t ParseMuPdfFile(Book *book, const char *path);
uint8_t IndexMuPdfMetadata(Book *book, const char *path);
