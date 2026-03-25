// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stdint.h>

class Book;
class Text;

uint8_t ParsePdfFile(Book *book, const char *path);
uint8_t IndexPdfMetadata(Book *book, const char *path);
