#pragma once

#include <stdint.h>
#include <string>

class Book;

uint8_t ParsePdfFile(Book *book, const char *path);
uint8_t IndexPdfMetadata(Book *book, const char *path);
int pdf_extract_cover(Book *book, const std::string &pdfpath);
