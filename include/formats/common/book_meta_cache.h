#pragma once

#include <stdint.h>
#include <string>

// Persistent disk cache for book metadata (title/author + extended fields and
// cover path).
// Keyed by book path + file size + mtime so stale entries are ignored
// automatically when the source file changes.
//
// Format: 6-byte header (magic + version) followed by three
// length-prefixed UTF-8 strings in this order:
// title, author, publisher, published, subjects, description,
// cover_image_path.
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
bool Save(const std::string &cache_path, const MetaEntry &entry);

// Reads a MetaEntry from disk. Returns true on success.
bool Load(const std::string &cache_path, MetaEntry *out);

// Creates the cache directory if it does not already exist.
void EnsureDir();

} // namespace book_meta_cache
