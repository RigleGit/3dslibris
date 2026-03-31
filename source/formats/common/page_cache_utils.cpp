#include "formats/common/page_cache_utils.h"

#include "string_utils.h"

#include <algorithm>
#include <stdio.h>

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
  if (!fp)
    return false;
  if (value.empty())
    return true;
  return fwrite(value.data(), 1, value.size(), fp) == value.size();
}

bool ReadRawString(FILE *fp, size_t length, std::string *out) {
  if (!fp || !out)
    return false;
  out->clear();
  if (length == 0)
    return true;
  out->resize(length);
  return fread(&(*out)[0], 1, length, fp) == length;
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

  if (fwrite(&length, 1, sizeof(length), fp) != sizeof(length))
    return false;
  return WriteRawString(fp, bounded);
}

bool ReadLengthPrefixedString16(FILE *fp, uint16_t max_bytes, bool allow_empty,
                                std::string *out) {
  if (!fp || !out)
    return false;

  uint16_t length = 0;
  if (fread(&length, 1, sizeof(length), fp) != sizeof(length) ||
      length > max_bytes || (!allow_empty && length == 0)) {
    return false;
  }
  return ReadRawString(fp, length, out);
}

bool WritePages(FILE *fp, const std::vector<CachedPage> &pages,
                uint16_t max_page_bytes) {
  if (!fp)
    return false;

  std::vector<uint8_t> buffer;
  size_t total_size = 0;
  for (size_t i = 0; i < pages.size(); i++) {
    total_size += sizeof(uint16_t) + pages[i].size();
  }
  buffer.reserve(total_size);

  for (size_t i = 0; i < pages.size(); i++) {
    const CachedPage &page = pages[i];
    if (page.size() > max_page_bytes)
      return false;
    const uint16_t length = (uint16_t)page.size();
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&length);
    buffer.push_back(ptr[0]);
    buffer.push_back(ptr[1]);
    if (!page.empty()) {
      buffer.insert(buffer.end(), page.data(), page.data() + page.size());
    }
  }

  if (!buffer.empty()) {
    if (fwrite(buffer.data(), 1, buffer.size(), fp) != buffer.size())
      return false;
  }
  return true;
}

bool ReadPages(FILE *fp, uint32_t count, uint16_t max_page_bytes,
               std::vector<CachedPage> *pages) {
  if (!fp || !pages)
    return false;

  pages->clear();
  pages->reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    uint16_t length = 0;
    if (fread(&length, 1, sizeof(length), fp) != sizeof(length) ||
        length > max_page_bytes) {
      return false;
    }

    CachedPage page(length);
    if (length > 0 && fread(page.data(), 1, length, fp) != length)
      return false;
    pages->push_back(page);
  }
  return true;
}

bool WriteChapters(FILE *fp, const std::vector<CachedChapter> &chapters,
                   uint16_t max_title_bytes) {
  if (!fp)
    return false;

  std::vector<uint8_t> buffer;
  size_t total_size = 0;
  for (size_t i = 0; i < chapters.size(); i++) {
    // page (2 bytes) + level (1 byte)
    total_size += 3;
    // title length (2 bytes) + title bytes
    uint16_t length = (uint16_t)ClampString(chapters[i].title, max_title_bytes).size();
    total_size += 2 + length;
  }
  buffer.reserve(total_size);

  for (size_t i = 0; i < chapters.size(); i++) {
    const CachedChapter &chapter = chapters[i];

    // Write page
    const uint8_t *page_ptr = reinterpret_cast<const uint8_t *>(&chapter.page);
    buffer.push_back(page_ptr[0]);
    buffer.push_back(page_ptr[1]);

    // Write level
    buffer.push_back(chapter.level);

    // Write title
    const std::string bounded = ClampString(chapter.title, max_title_bytes);
    const uint16_t length = (uint16_t)bounded.size();
    const uint8_t *len_ptr = reinterpret_cast<const uint8_t *>(&length);
    buffer.push_back(len_ptr[0]);
    buffer.push_back(len_ptr[1]);
    if (!bounded.empty()) {
      buffer.insert(buffer.end(), bounded.data(), bounded.data() + bounded.size());
    }
  }

  if (!buffer.empty()) {
    if (fwrite(buffer.data(), 1, buffer.size(), fp) != buffer.size())
      return false;
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

} // namespace page_cache_utils
