#include "formats/common/book_meta_cache.h"

#include "formats/common/binary_io_utils.h"
#include "shared/path_constants.h"
#include "shared/string_utils.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace book_meta_cache {

namespace {

static const uint32_t kMagic   = 0x434D3342U; // 'B3MC'
static const uint16_t kVersion = 3;

struct Header {
  uint32_t magic;
  uint16_t version;
};

static const uint32_t kIndexMagic = 0x494D3342U; // 'B3MI'
static const uint16_t kIndexVersion = 1;
static const size_t kIndexMemoryLimit = 512 * 1024;
static const long kIndexFileLimit = 2 * 1024 * 1024;

static void Fields(const MetaEntry &entry, const std::string **fields) {
  fields[0] = &entry.title; fields[1] = &entry.author;
  fields[2] = &entry.series; fields[3] = &entry.language;
  fields[4] = &entry.publisher; fields[5] = &entry.published;
  fields[6] = &entry.subjects; fields[7] = &entry.description;
  fields[8] = &entry.cover_image_path;
}

static size_t EntryBytes(const MetaEntry &entry) {
  const std::string *fields[9]; Fields(entry, fields);
  size_t bytes = sizeof(MetaEntry) + 64;
  for (size_t i = 0; i < 9; ++i) bytes += fields[i]->size() + 1;
  return bytes;
}

static bool CacheKey(const std::string &path, uint64_t *key) {
  const std::string prefix = paths::GetMetaCacheDir() + "/";
  if (path.size() != prefix.size() + 20 || path.compare(0, prefix.size(), prefix) ||
      path.compare(path.size() - 4, 4, ".bmc")) return false;
  const std::string hex = path.substr(prefix.size(), 16);
  for (size_t i = 0; i < hex.size(); ++i)
    if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
      return false;
  *key = strtoull(hex.c_str(), NULL, 16);
  return true;
}

static uint64_t RecordHash(uint64_t key, const MetaEntry &entry) {
  uint64_t hash = 14695981039346656037ULL;
  const std::string *fields[9]; Fields(entry, fields);
  for (int shift = 0; shift < 64; shift += 8) {
    hash ^= (key >> shift) & 255; hash *= 1099511628211ULL;
  }
  for (size_t i = 0; i < 9; ++i) {
    for (unsigned char c : *fields[i]) { hash ^= c; hash *= 1099511628211ULL; }
    hash ^= 0; hash *= 1099511628211ULL;
  }
  return hash;
}

static bool WriteIndexHeader(FILE *fp) {
  return binary_io_utils::WriteRaw(fp, &kIndexMagic, sizeof(kIndexMagic)) &&
         binary_io_utils::WriteRaw(fp, &kIndexVersion, sizeof(kIndexVersion));
}

static bool ReadIndexHeader(FILE *fp) {
  uint32_t magic = 0; uint16_t version = 0;
  return binary_io_utils::ReadRaw(fp, &magic, sizeof(magic)) &&
         binary_io_utils::ReadRaw(fp, &version, sizeof(version)) &&
         magic == kIndexMagic && version == kIndexVersion;
}

static bool WriteRecord(FILE *fp, uint64_t key, const MetaEntry &entry) {
  const std::string *fields[9]; Fields(entry, fields);
  bool ok = binary_io_utils::WriteRaw(fp, &key, sizeof(key));
  for (size_t i = 0; i < 9 && ok; ++i)
    ok = binary_io_utils::WriteString32(fp, *fields[i], 4096);
  const uint64_t hash = RecordHash(key, entry);
  return ok && binary_io_utils::WriteRaw(fp, &hash, sizeof(hash));
}

static bool ReadRecord(FILE *fp, uint64_t *key, MetaEntry *entry) {
  std::string *fields[] = {&entry->title, &entry->author, &entry->series,
      &entry->language, &entry->publisher, &entry->published, &entry->subjects,
      &entry->description, &entry->cover_image_path};
  bool ok = binary_io_utils::ReadRaw(fp, key, sizeof(*key));
  for (size_t i = 0; i < 9 && ok; ++i)
    ok = binary_io_utils::ReadString32(fp, 4096, fields[i]);
  uint64_t hash = 0;
  return ok && binary_io_utils::ReadRaw(fp, &hash, sizeof(hash)) &&
         hash == RecordHash(*key, *entry);
}

// Metadata jobs already write individual caches. Append only their changed
// record; do not rewrite the whole library for every indexed book. A later
// sorting batch compacts the bounded journal when it learns missing records.
static void AppendIndexRecord(const std::string &cache_path, const MetaEntry &entry) {
  uint64_t key;
  if (!CacheKey(cache_path, &key)) return;
  const std::string &path = paths::GetMetaIndexPath();
  FILE *fp = fopen(path.c_str(), "r+b");
  if (!fp) remove(path.c_str());
  if (fp && (!ReadIndexHeader(fp) || fseek(fp, 0, SEEK_END) ||
             ftell(fp) < 0 || ftell(fp) >= kIndexFileLimit)) {
    fclose(fp); fp = NULL;
    // Invalid/oversized indices are disposable; .bmc remains authoritative.
    remove(path.c_str());
  }
  if (!fp) {
    fp = fopen(path.c_str(), "wb");
    if (!fp) return;
    if (!WriteIndexHeader(fp)) { fclose(fp); remove(path.c_str()); return; }
  }
  const bool ok = WriteRecord(fp, key, entry);
  const bool closed = fclose(fp) == 0;
  if (!ok || !closed) remove(path.c_str());
}

} // namespace

std::string BuildPath(const std::string &book_path, long long file_size,
                      long long file_mtime) {
  std::string key = "src2|" + book_path;
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

bool Save(const std::string &cache_path, const MetaEntry &entry, bool prepared) {
  if (cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return false;

  Header hdr{};
  hdr.magic   = kMagic;
  hdr.version = prepared ? 4 : kVersion;

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
  const bool closed = fclose(fp) == 0;
  if (ok && closed) {
    AppendIndexRecord(cache_path, entry);
  } else {
    uint64_t key;
    if (CacheKey(cache_path, &key)) remove(paths::GetMetaIndexPath().c_str());
  }
  return ok && closed;
}

bool Load(const std::string &cache_path, MetaEntry *out, bool *prepared) {
  if (prepared) *prepared = false;
  if (!out || cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;

  Header hdr;
  if (!binary_io_utils::ReadRaw(fp, &hdr, sizeof(hdr)) ||
      hdr.magic != kMagic || (hdr.version != kVersion && hdr.version != 4)) {
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
  if (ok && prepared) *prepared = hdr.version == 4;
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

Index::Index() : bytes_(0), dirty_(false) {
  FILE *fp = fopen(paths::GetMetaIndexPath().c_str(), "rb");
  if (!fp) return;
  bool ok = fseek(fp, 0, SEEK_END) == 0;
  const long length = ok ? ftell(fp) : -1;
  ok = ok && length >= 6 && length <= kIndexFileLimit &&
       fseek(fp, 0, SEEK_SET) == 0 && ReadIndexHeader(fp);
  // EOF probing stays inside stdio's sequential buffer. ftell may consult
  // the underlying SD stream, so avoid asking for its position per record.
  while (ok) {
    const int next = fgetc(fp);
    if (next == EOF) { ok = !ferror(fp); break; }
    if (ungetc(next, fp) == EOF) { ok = false; break; }
    uint64_t key = 0; MetaEntry entry;
    ok = ReadRecord(fp, &key, &entry);
    if (ok) Remember(key, entry);
  }
  fclose(fp);
  if (!ok) { entries_.clear(); bytes_ = 0; }
  // An invalid or interrupted file is rebuilt from independently valid .bmc
  // records on demand. Never serve the valid prefix of a corrupt journal.
  dirty_ = false;
}

void Index::Remember(uint64_t key, const MetaEntry &entry) {
  const size_t bytes = EntryBytes(entry);
  if (bytes > kIndexMemoryLimit) return;
  auto old = entries_.find(key);
  if (old != entries_.end()) {
    bytes_ -= EntryBytes(old->second); entries_.erase(old);
  }
  while (!entries_.empty() && bytes_ + bytes > kIndexMemoryLimit) {
    bytes_ -= EntryBytes(entries_.begin()->second);
    entries_.erase(entries_.begin());
  }
  entries_[key] = entry;
  bytes_ += bytes;
}

bool Index::Load(const std::string &path, MetaEntry *out) {
  if (!out) return false;
  uint64_t key = 0;
  const bool keyed = CacheKey(path, &key);
  if (keyed) {
    const auto found = entries_.find(key);
    if (found != entries_.end()) { *out = found->second; return true; }
  }
  if (!book_meta_cache::Load(path, out)) return false;
  if (keyed) { Remember(key, *out); dirty_ = true; }
  return true;
}

Index::~Index() {
  if (!dirty_) return;
  EnsureDir();
  const std::string path = paths::GetMetaIndexPath();
  const std::string temp = path + ".tmp";
  FILE *fp = fopen(temp.c_str(), "wb");
  if (!fp) return;
  bool ok = WriteIndexHeader(fp);
  for (const auto &entry : entries_)
    if (ok) ok = WriteRecord(fp, entry.first, entry.second);
  const bool closed = fclose(fp) == 0;
  if (!ok || !closed || rename(temp.c_str(), path.c_str())) remove(temp.c_str());
}

} // namespace book_meta_cache
