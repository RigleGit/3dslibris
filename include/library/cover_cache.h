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

void ClearPathCache();
// Manual preload retains all disk thumbnails; decoded images remain caller-owned.
bool EnableFullRetention();
void ResetMaintenance();
bool TryLoadCachedOnly(Book *book, const std::string &book_path);

bool TryLoad(Book *book, const std::string &book_path);
bool Save(Book *book, const std::string &book_path);
// A verified "this source has no cover" record, keyed like the thumbnail so a
// changed book or sidecar invalidates it. It spares repeated extractor runs.
bool MarkAbsent(Book *book, const std::string &book_path);
bool IsMarkedAbsent(Book *book, const std::string &book_path);
bool HasAdjacentOverride(Book *book, const std::string &book_path);
bool TryLoadAdjacentOverride(Book *book, const std::string &book_path);

} // namespace cover_cache
