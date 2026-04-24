#include "formats/common/book_meta_cache.h"

#include "path_utils.h"
#include "shared/string_utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace book_meta_cache {

namespace {

static const uint32_t kMagic   = 0x434D3342U; // 'B3MC'
static const uint16_t kVersion = 1;

struct Header {
  uint32_t magic;
  uint16_t version;
};

// Write a length-prefixed string (4-byte LE length + raw bytes).
static bool WriteStr(FILE *fp, const std::string &s) {
  uint32_t len = (uint32_t)s.size();
  if (fwrite(&len, sizeof(len), 1, fp) != 1)
    return false;
  if (len > 0 && fwrite(s.data(), 1, len, fp) != len)
    return false;
  return true;
}

// Read a length-prefixed string. Returns false on I/O error or if the
// stored length exceeds max_len (guards against corrupt files).
static bool ReadStr(FILE *fp, std::string *out, uint32_t max_len) {
  uint32_t len = 0;
  if (fread(&len, sizeof(len), 1, fp) != 1)
    return false;
  if (len > max_len)
    return false;
  if (len == 0) {
    out->clear();
    return true;
  }
  out->resize(len);
  return fread(&(*out)[0], 1, len, fp) == len;
}

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

  bool ok = fwrite(&hdr, sizeof(hdr), 1, fp) == 1;
  ok = ok && WriteStr(fp, entry.title);
  ok = ok && WriteStr(fp, entry.author);
  ok = ok && WriteStr(fp, entry.cover_image_path);
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
  if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.magic != kMagic ||
      hdr.version != kVersion) {
    fclose(fp);
    return false;
  }

  // 4096 bytes is a generous upper bound for title/author; cover path
  // inside an EPUB zip is typically much shorter.
  const uint32_t kMaxStrLen = 4096;
  bool ok = ReadStr(fp, &out->title, kMaxStrLen) &&
            ReadStr(fp, &out->author, kMaxStrLen) &&
            ReadStr(fp, &out->cover_image_path, kMaxStrLen);
  fclose(fp);
  if (!ok) {
    out->title.clear();
    out->author.clear();
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
