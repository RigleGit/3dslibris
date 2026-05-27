/*
    3dslibris - cover_cache.h
    New 3DS library module by Rigle.

    Summary:
    - Disk I/O for the browser cover thumbnail cache (.cvr files on SD).
    - Extracted from library/app_browser.cpp to isolate cache I/O from browser UI.
*/

#pragma once

#include <string>
#include <stdint.h>

class Book;

namespace cover_cache {

static const uint8_t kMaxAttempts = 3;

bool TryLoad(Book *book, const std::string &book_path);
bool Save(Book *book, const std::string &book_path);
bool TryLoadAdjacentOverride(Book *book, const std::string &book_path);

} // namespace cover_cache
