#pragma once

#include <stdint.h>
#include <string>
#include <map>

// Persistent disk cache for book metadata (title/author + extended fields and
// cover path).
// Keyed by book path + file size + mtime so stale entries are ignored
// automatically when the source file changes.
//
// Version 3 remains readable for library display; version 4 certifies genuine
// title and author preparation (including empty fields). The native header is
// followed by nine length-prefixed UTF-8 fields in MetaEntry order.
// An empty string is stored as a 4-byte zero length.

namespace book_meta_cache {

struct MetaEntry {
  std::string title;
  std::string author;
  std::string series;
  std::string language;
  std::string publisher;
  std::string published;
  std::string subjects;
  std::string description;
  std::string cover_image_path; // EPUB: path inside the ZIP archive

  MetaEntry() {}
};

// Returns the cache file path for a given book.
std::string BuildPath(const std::string &book_path, long long file_size,
                      long long file_mtime);

// Writes a MetaEntry to disk. Returns true on success.
bool Save(const std::string &cache_path, const MetaEntry &entry,
          bool prepared = false);

// Reads a MetaEntry from disk. Returns true on success.
bool Load(const std::string &cache_path, MetaEntry *out,
          bool *prepared = nullptr);

// Creates the cache directory if it does not already exist.
void EnsureDir();

// Short-lived during library sorting. Keys retain the existing path/size/mtime
// identity; the full metadata payload preserves later metadata/cover consumers.
// Resident entries are bounded to 512 KiB and discarded when the sort ends.
class Index {
public:
  Index();
  ~Index();
  bool Load(const std::string &cache_path, MetaEntry *out);
private:
  std::map<uint64_t, MetaEntry> entries_;
  size_t bytes_;
  bool dirty_;
  void Remember(uint64_t key, const MetaEntry &entry);
  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;
};

} // namespace book_meta_cache
