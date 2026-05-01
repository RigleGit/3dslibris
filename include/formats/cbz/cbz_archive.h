#pragma once

#include "formats/cbz/cbz_types.h"

#include <string>
#include <vector>

struct CbzComicInfoBookmark {
  int image_index;
  std::string title;

  CbzComicInfoBookmark() : image_index(-1), title() {}
};

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries);
bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes);
bool ReadComicInfoBookmarks(const std::string &archive_path,
                            std::vector<CbzComicInfoBookmark> *out);
const char *GetLastCbzArchiveError();
