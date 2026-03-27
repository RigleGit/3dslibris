#pragma once

#include <stdint.h>

class Book;

uint8_t ParsePdfFile(Book *book, const char *path);
uint8_t IndexPdfMetadata(Book *book, const char *path);
