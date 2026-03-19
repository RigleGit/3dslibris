#pragma once

#include <stdint.h>
#include <string>

namespace mobi_cover_meta_cache {

enum ResultKind {
  kUnknown = 0,
  kNoCover = 1,
  kCandidate = 2
};

struct CoverMeta {
  ResultKind kind;
  uint32_t record_index;
  uint32_t record_start;
  uint32_t record_end;
  uint32_t image_offset;
  uint32_t width;
  uint32_t height;

  CoverMeta()
      : kind(kUnknown), record_index(0), record_start(0), record_end(0),
        image_offset(0), width(0), height(0) {}
};

std::string BuildPath(const std::string &book_path, long long file_size,
                      long long file_mtime);
bool Save(const std::string &path, const CoverMeta &meta);
bool Load(const std::string &path, CoverMeta *out);

} // namespace mobi_cover_meta_cache
