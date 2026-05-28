#include "formats/common/book_meta_cache.h"

#include "formats/common/binary_io_utils.h"
#include "shared/path_constants.h"
#include "shared/string_utils.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

namespace book_meta_cache {

namespace {

static const uint32_t kMagic   = 0x434D3342U; // 'B3MC'
static const uint16_t kVersion = 3;

struct Header {
  uint32_t magic;
  uint16_t version;
};

} // namespace

std::string BuildPath(const std::string &book_path, long long file_size,
                      long long file_mtime) {
  std::string key = book_path;
  key.push_back('|');
  key += std::to_string(file_size);
  key.push_back('|');
  key += std::to_string(file_mtime);
  uint64_t h = Fnv1a64(key);
  char out[256];
  snprintf(out, sizeof(out), "%s/%016llx.bmc", paths::GetMetaCacheDir().c_str(),
           (unsigned long long)h);
  return std::string(out);
}

bool Save(const std::string &cache_path, const MetaEntry &entry) {
  if (cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return false;

  Header hdr;
  hdr.magic   = kMagic;
  hdr.version = kVersion;

  bool ok = binary_io_utils::WriteRaw(fp, &hdr, sizeof(hdr));
  ok = ok && binary_io_utils::WriteString32(fp, entry.title, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.author, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.series, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.language, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.publisher, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.published, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.subjects, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.description, UINT32_MAX);
  ok = ok && binary_io_utils::WriteString32(fp, entry.cover_image_path,
                                            UINT32_MAX);
  fclose(fp);
  return ok;
}

bool Load(const std::string &cache_path, MetaEntry *out) {
  if (!out || cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;

  Header hdr;
  if (!binary_io_utils::ReadRaw(fp, &hdr, sizeof(hdr)) ||
      hdr.magic != kMagic || hdr.version != kVersion) {
    fclose(fp);
    return false;
  }

  // 4096 bytes is a generous upper bound for title/author; cover path
  // inside an EPUB zip is typically much shorter.
  const uint32_t kMaxStrLen = 4096;
  bool ok = binary_io_utils::ReadString32(fp, kMaxStrLen, &out->title) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->author) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->series) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->language) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->publisher) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->published) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->subjects) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen, &out->description) &&
            binary_io_utils::ReadString32(fp, kMaxStrLen,
                                          &out->cover_image_path);
  fclose(fp);
  if (!ok) {
    out->title.clear();
    out->author.clear();
    out->series.clear();
    out->language.clear();
    out->publisher.clear();
    out->published.clear();
    out->subjects.clear();
    out->description.clear();
    out->cover_image_path.clear();
  }
  return ok;
}

void EnsureDir() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(paths::GetCacheBaseDir().c_str(), 0777);
  mkdir(paths::GetMetaCacheDir().c_str(), 0777);
  initialized = true;
}

} // namespace book_meta_cache
