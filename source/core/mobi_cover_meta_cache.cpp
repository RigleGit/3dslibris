#include "mobi_cover_meta_cache.h"

#include "string_utils.h"

#include <stdio.h>
#include <string.h>

namespace mobi_cover_meta_cache {
namespace {

static const uint32_t kMagic = 0x4D434D43U; // MCMC
static const uint16_t kVersion = 1;
static const char *kBaseDir = "sdmc:/3ds/3dslibris/cache/mobi-cover";

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t kind;
  uint32_t record_index;
  uint32_t record_start;
  uint32_t record_end;
  uint32_t image_offset;
  uint32_t width;
  uint32_t height;
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
  char out[192];
  snprintf(out, sizeof(out), "%s/%016llx.mcm",
           kBaseDir, (unsigned long long)h);
  return std::string(out);
}

bool Save(const std::string &path, const CoverMeta &meta) {
  if (path.empty())
    return false;

  Header hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kMagic;
  hdr.version = kVersion;
  hdr.kind = (uint16_t)meta.kind;
  hdr.record_index = meta.record_index;
  hdr.record_start = meta.record_start;
  hdr.record_end = meta.record_end;
  hdr.image_offset = meta.image_offset;
  hdr.width = meta.width;
  hdr.height = meta.height;

  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp)
    return false;
  const bool ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  fclose(fp);
  return ok;
}

bool Load(const std::string &path, CoverMeta *out) {
  if (!out || path.empty())
    return false;

  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
    return false;

  Header hdr;
  const bool ok = fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  fclose(fp);
  if (!ok || hdr.magic != kMagic || hdr.version != kVersion ||
      hdr.kind > (uint16_t)kCandidate) {
    return false;
  }

  out->kind = (ResultKind)hdr.kind;
  out->record_index = hdr.record_index;
  out->record_start = hdr.record_start;
  out->record_end = hdr.record_end;
  out->image_offset = hdr.image_offset;
  out->width = hdr.width;
  out->height = hdr.height;
  return true;
}

} // namespace mobi_cover_meta_cache
