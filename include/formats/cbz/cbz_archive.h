#pragma once

#include "formats/cbz/cbz_types.h"

#include <string>
#include <vector>

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries);
bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes);
