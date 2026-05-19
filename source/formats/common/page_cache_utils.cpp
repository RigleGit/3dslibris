#include "formats/common/page_cache_utils.h"

#include "formats/common/binary_io_utils.h"
#include "shared/string_utils.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

namespace page_cache_utils {

CachedChapter::CachedChapter() : page(0), level(0), title() {}

CachedChapter::CachedChapter(uint16_t page, uint8_t level,
                             const std::string &title)
    : page(page), level(level), title(title) {}

std::string BuildPageCachePath(const std::string &cache_dir,
                               const std::string &extension,
                               const std::string &book_path,
                               const PageCacheLayoutParams &params) {
  if (cache_dir.empty() || extension.empty() || book_path.empty())
    return std::string();

  std::string key(book_path);
  key.push_back('|');
  key += std::to_string(params.file_size);
  key.push_back('|');
  key += std::to_string(params.file_mtime);
  key.push_back('|');
  key += std::to_string(params.pixel_size);
  key.push_back('|');
  key += std::to_string(params.line_spacing);
  key.push_back('|');
  key += std::to_string(params.paragraph_spacing);
  key.push_back('|');
  key += std::to_string(params.paragraph_indent);
  key.push_back('|');
  key += std::to_string(params.orientation);
  key.push_back('|');
  key += std::to_string(params.margin_left);
  key.push_back('|');
  key += std::to_string(params.margin_right);
  key.push_back('|');
  key += std::to_string(params.margin_top);
  key.push_back('|');
  key += std::to_string(params.margin_bottom);
  key.push_back('|');
  key += params.regular_font;
  if (!params.variant_token.empty()) {
    key.push_back('|');
    key += params.variant_token;
  }

  uint64_t hash = Fnv1a64(key);
  char out[192];
  snprintf(out, sizeof(out), "%s/%016llx%s", cache_dir.c_str(),
           (unsigned long long)hash, extension.c_str());
  return std::string(out);
}

std::string ClampString(const std::string &value, size_t max_bytes) {
  if (value.size() <= max_bytes)
    return value;
  return value.substr(0, max_bytes);
}

bool WriteRawString(FILE *fp, const std::string &value) {
  return binary_io_utils::WriteRaw(fp, value.data(), value.size());
}

bool ReadRawString(FILE *fp, size_t length, std::string *out) {
  if (!out)
    return false;
  out->clear();
  if (length == 0)
    return true;
  out->resize(length);
  return binary_io_utils::ReadRaw(fp, &(*out)[0], length);
}

bool WriteLengthPrefixedString16(FILE *fp, const std::string &value,
                                 uint16_t max_bytes, bool allow_empty,
                                 uint16_t *written_length) {
  if (!fp)
    return false;

  const std::string bounded = ClampString(value, max_bytes);
  const uint16_t length = (uint16_t)bounded.size();
  if (!allow_empty && length == 0)
    return false;
  if (written_length)
    *written_length = length;

  if (!binary_io_utils::WriteRaw(fp, &length, sizeof(length)))
    return false;
  return WriteRawString(fp, bounded);
}

bool ReadLengthPrefixedString16(FILE *fp, uint16_t max_bytes, bool allow_empty,
                                std::string *out) {
  if (!fp || !out)
    return false;

  uint16_t length = 0;
  if (!binary_io_utils::ReadRaw(fp, &length, sizeof(length)) ||
      length > max_bytes || (!allow_empty && length == 0)) {
    return false;
  }
  return ReadRawString(fp, length, out);
}

bool WritePages(FILE *fp, const std::vector<CachedPage> &pages,
                uint16_t max_page_codepoints) {
  if (!fp)
    return false;

  for (size_t i = 0; i < pages.size(); i++) {
    const CachedPage &page = pages[i];
    if (page.size() > max_page_codepoints)
      return false;
    const uint16_t length = (uint16_t)page.size();
    if (fwrite(&length, 1, sizeof(length), fp) != sizeof(length))
      return false;
    if (!page.empty()) {
      const size_t byte_count = page.size() * sizeof(uint32_t);
      if (fwrite(page.data(), 1, byte_count, fp) != byte_count)
        return false;
    }
  }
  return true;
}

bool ReadPages(FILE *fp, uint32_t count, uint16_t max_page_codepoints,
               std::vector<CachedPage> *pages) {
  if (!fp || !pages)
    return false;

  pages->clear();
  pages->reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    uint16_t length = 0;
    if (fread(&length, 1, sizeof(length), fp) != sizeof(length) ||
        length > max_page_codepoints) {
      return false;
    }

    CachedPage page(length);
    const size_t byte_count = (size_t)length * sizeof(uint32_t);
    if (length > 0 && fread(page.data(), 1, byte_count, fp) != byte_count)
      return false;
    pages->push_back(page);
  }
  return true;
}

bool WriteChapters(FILE *fp, const std::vector<CachedChapter> &chapters,
                   uint16_t max_title_bytes) {
  if (!fp)
    return false;

  for (size_t i = 0; i < chapters.size(); i++) {
    const CachedChapter &chapter = chapters[i];

    if (fwrite(&chapter.page, 1, sizeof(chapter.page), fp) != sizeof(chapter.page))
      return false;
    if (fwrite(&chapter.level, 1, sizeof(chapter.level), fp) != sizeof(chapter.level))
      return false;

    const std::string bounded = ClampString(chapter.title, max_title_bytes);
    const uint16_t length = (uint16_t)bounded.size();
    if (fwrite(&length, 1, sizeof(length), fp) != sizeof(length))
      return false;
    if (!bounded.empty()) {
      if (fwrite(bounded.data(), 1, bounded.size(), fp) != bounded.size())
        return false;
    }
  }
  return true;
}

bool ReadChapters(FILE *fp, uint32_t count, uint16_t max_title_bytes,
                  std::vector<CachedChapter> *chapters) {
  if (!fp || !chapters)
    return false;

  chapters->clear();
  chapters->reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    CachedChapter chapter;
    if (fread(&chapter.page, 1, sizeof(chapter.page), fp) !=
            sizeof(chapter.page) ||
        fread(&chapter.level, 1, sizeof(chapter.level), fp) !=
            sizeof(chapter.level) ||
        !ReadLengthPrefixedString16(fp, max_title_bytes, true,
                                    &chapter.title)) {
      return false;
    }
    chapters->push_back(chapter);
  }
  return true;
}

// ---------- Cursor-based (memory buffer) variants ----------
//
// Eliminate the stdio overhead that dominates cache-hit time on large
// EPUBs. Wire format is identical to the FILE* path — these helpers walk a
// pre-fetched buffer with bounds-checked pointer reads.

bool ReadRawString(BufReader *r, size_t length, std::string *out) {
  if (!r || !out)
    return false;
  out->clear();
  if (length == 0)
    return true;
  if (!r->Remaining(length))
    return false;
  out->assign((const char *)r->cur, length);
  r->cur += length;
  return true;
}

bool ReadLengthPrefixedString16(BufReader *r, uint16_t max_bytes,
                                bool allow_empty, std::string *out) {
  if (!r || !out)
    return false;
  uint16_t length = 0;
  if (!r->ReadRaw(&length, sizeof(length)) || length > max_bytes ||
      (!allow_empty && length == 0)) {
    return false;
  }
  return ReadRawString(r, length, out);
}

bool ReadPages(BufReader *r, uint32_t count, uint16_t max_page_codepoints,
               std::vector<CachedPage> *pages) {
  if (!r || !pages)
    return false;
  pages->clear();
  pages->reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    uint16_t length = 0;
    if (!r->ReadRaw(&length, sizeof(length)) || length > max_page_codepoints)
      return false;
    CachedPage page(length);
    const size_t byte_count = (size_t)length * sizeof(uint32_t);
    if (length > 0) {
      if (!r->Remaining(byte_count))
        return false;
      memcpy(page.data(), r->cur, byte_count);
      r->cur += byte_count;
    }
    pages->push_back(std::move(page));
  }
  return true;
}

bool ReadChapters(BufReader *r, uint32_t count, uint16_t max_title_bytes,
                  std::vector<CachedChapter> *chapters) {
  if (!r || !chapters)
    return false;
  chapters->clear();
  chapters->reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    CachedChapter chapter;
    if (!r->ReadRaw(&chapter.page, sizeof(chapter.page)) ||
        !r->ReadRaw(&chapter.level, sizeof(chapter.level)) ||
        !ReadLengthPrefixedString16(r, max_title_bytes, true, &chapter.title)) {
      return false;
    }
    chapters->push_back(chapter);
  }
  return true;
}

// ---------- BufWriter helpers (in-memory body serialization) ----------

bool WriteRawString(BufWriter *w, const std::string &value) {
  if (!w)
    return false;
  return w->WriteRaw(value.data(), value.size());
}

bool WriteLengthPrefixedString16(BufWriter *w, const std::string &value,
                                 uint16_t max_bytes, bool allow_empty,
                                 uint16_t *written_length) {
  if (!w)
    return false;
  const std::string bounded = ClampString(value, max_bytes);
  const uint16_t length = (uint16_t)bounded.size();
  if (!allow_empty && length == 0)
    return false;
  if (written_length)
    *written_length = length;
  if (!w->WriteRaw(&length, sizeof(length)))
    return false;
  return WriteRawString(w, bounded);
}

bool WritePages(BufWriter *w, const std::vector<CachedPage> &pages,
                uint16_t max_page_codepoints) {
  if (!w)
    return false;
  for (size_t i = 0; i < pages.size(); i++) {
    const CachedPage &page = pages[i];
    if (page.size() > max_page_codepoints)
      return false;
    const uint16_t length = (uint16_t)page.size();
    if (!w->WriteRaw(&length, sizeof(length)))
      return false;
    if (length > 0 && !w->WriteRaw(page.data(), page.size() * sizeof(uint32_t)))
      return false;
  }
  return true;
}

bool WriteChapters(BufWriter *w, const std::vector<CachedChapter> &chapters,
                   uint16_t max_title_bytes) {
  if (!w)
    return false;
  for (size_t i = 0; i < chapters.size(); i++) {
    const CachedChapter &chapter = chapters[i];
    if (!w->WriteRaw(&chapter.page, sizeof(chapter.page)) ||
        !w->WriteRaw(&chapter.level, sizeof(chapter.level))) {
      return false;
    }
    const std::string bounded = ClampString(chapter.title, max_title_bytes);
    const uint16_t length = (uint16_t)bounded.size();
    if (!w->WriteRaw(&length, sizeof(length)))
      return false;
    if (!bounded.empty() && !w->WriteRaw(bounded.data(), bounded.size()))
      return false;
  }
  return true;
}

// ---------- zlib (de)compress for the page-cache body ----------

bool CompressBody(const std::vector<uint8_t> &body,
                  std::vector<uint8_t> *compressed) {
  if (!compressed)
    return false;
  compressed->clear();
  if (body.empty())
    return true;
  uLongf bound = compressBound((uLong)body.size());
  compressed->resize((size_t)bound);
  uLongf out_size = bound;
  // Level 6 (default) on 3DS: ~10MB/s decompress, ~2-4MB/s compress.
  // Cache files compress to ~30-50% original on Latin-heavy text — the
  // payoff is read-time SD bandwidth savings.
  int rc = compress2(compressed->data(), &out_size, body.data(),
                     (uLong)body.size(), Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) {
    compressed->clear();
    return false;
  }
  compressed->resize((size_t)out_size);
  return true;
}

bool DecompressBody(const uint8_t *compressed_data, size_t compressed_size,
                    size_t uncompressed_size, std::vector<uint8_t> *out) {
  if (!out || !compressed_data || compressed_size == 0)
    return false;
  out->resize(uncompressed_size);
  uLongf dest_len = (uLongf)uncompressed_size;
  int rc = uncompress(out->data(), &dest_len, compressed_data,
                      (uLong)compressed_size);
  if (rc != Z_OK || dest_len != (uLongf)uncompressed_size) {
    out->clear();
    return false;
  }
  return true;
}

} // namespace page_cache_utils
