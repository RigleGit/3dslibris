/*
    3dslibris - epub_zip_utils.h
    ZIP archive utilities for EPUB parsing.
    Extracted from epub.cpp by Rigle.
*/

#pragma once

#include <string>
#include <unordered_map>
#include "minizip/unzip.h"

namespace epub_zip_utils {

struct ZipEntryIndex {
  bool built = false;
  std::unordered_map<std::string, uLong> offset_by_key;
};

bool LocateSafe(unzFile uf, const std::string &entry_path,
                ZipEntryIndex *index = NULL);

bool ReadText(unzFile uf, const std::string &path, std::string &out,
              size_t max_bytes, ZipEntryIndex *index = NULL);

} // namespace epub_zip_utils
