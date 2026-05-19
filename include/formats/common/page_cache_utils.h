#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace page_cache_utils {

struct PageCacheLayoutParams {
  long long file_size;
  long long file_mtime;
  int pixel_size;
  int line_spacing;
  int paragraph_spacing;
  int paragraph_indent;
  int orientation;
  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;
  std::string regular_font;
  std::string variant_token;

  PageCacheLayoutParams()
      : file_size(0), file_mtime(0), pixel_size(0), line_spacing(0),
        paragraph_spacing(0), paragraph_indent(0), orientation(0),
        margin_left(0), margin_right(0), margin_top(0), margin_bottom(0) {}
};

typedef std::vector<uint32_t> CachedPage;

struct CachedChapter {
  uint16_t page;
  uint8_t level;
  std::string title;

  CachedChapter();
  CachedChapter(uint16_t page, uint8_t level, const std::string &title);
};

std::string BuildPageCachePath(const std::string &cache_dir,
                               const std::string &extension,
                               const std::string &book_path,
                               const PageCacheLayoutParams &params);

std::string ClampString(const std::string &value, size_t max_bytes);
bool WriteRawString(FILE *fp, const std::string &value);
bool ReadRawString(FILE *fp, size_t length, std::string *out);

bool WriteLengthPrefixedString16(FILE *fp, const std::string &value,
                                 uint16_t max_bytes, bool allow_empty = true,
                                 uint16_t *written_length = NULL);
bool ReadLengthPrefixedString16(FILE *fp, uint16_t max_bytes, bool allow_empty,
                                std::string *out);

bool WritePages(FILE *fp, const std::vector<CachedPage> &pages,
                uint16_t max_page_codepoints);
bool ReadPages(FILE *fp, uint32_t count, uint16_t max_page_codepoints,
               std::vector<CachedPage> *pages);

bool WriteChapters(FILE *fp, const std::vector<CachedChapter> &chapters,
                   uint16_t max_title_bytes);
bool ReadChapters(FILE *fp, uint32_t count, uint16_t max_title_bytes,
                  std::vector<CachedChapter> *chapters);

// Cursor-based variants. Same wire format as the FILE* path above; reading
// from a pre-fetched memory buffer avoids the per-record stdio overhead that
// dominates cache-hit time on large EPUBs (~1.3ms per page via fread()).
struct BufReader {
  const uint8_t *cur;
  const uint8_t *end;

  BufReader(const uint8_t *data, size_t size)
      : cur(data), end(data + size) {}

  bool Remaining(size_t n) const {
    return (size_t)(end - cur) >= n;
  }
  bool ReadRaw(void *dst, size_t n) {
    if (!Remaining(n))
      return false;
    if (n)
      memcpy(dst, cur, n);
    cur += n;
    return true;
  }
};

bool ReadRawString(BufReader *r, size_t length, std::string *out);
bool ReadLengthPrefixedString16(BufReader *r, uint16_t max_bytes,
                                bool allow_empty, std::string *out);
bool ReadPages(BufReader *r, uint32_t count, uint16_t max_page_codepoints,
               std::vector<CachedPage> *pages);
bool ReadChapters(BufReader *r, uint32_t count, uint16_t max_title_bytes,
                  std::vector<CachedChapter> *chapters);

} // namespace page_cache_utils
