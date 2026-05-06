/*
    3dslibris - book_open_index.cpp
*/

#include "book/book.h"
#include "book/book_parser.h"

#include <sys/stat.h>

#include "formats/common/book_error.h"
#include "formats/common/book_meta_cache.h"

u8 Book::Open() {
  PrepareForOpen();
  return OpenPrepared();
}

// Tries to populate title/author/coverImagePath from the disk cache without
// opening the source file. Returns true on cache hit.
bool Book::TryLoadMetadataFromCache() {
  if (metadataIndexTried)
    return metadataIndexed;

  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  book_meta_cache::EnsureDir();
  struct stat st;
  long long fsize = 0, fmtime = 0;
  if (stat(path.c_str(), &st) == 0) {
    fsize  = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }
  book_meta_cache::MetaEntry cached;
  if (!book_meta_cache::Load(book_meta_cache::BuildPath(path, fsize, fmtime),
                             &cached))
    return false;

  if (!cached.title.empty())
    SetTitle(cached.title.c_str());
  if (!cached.author.empty())
    SetAuthor(cached.author);
  coverImagePath     = cached.cover_image_path;
  metadataIndexTried = true;
  metadataIndexed    = true;
  ClearBrowserDisplayNameCache();
  return true;
}

u8 Book::Index() {
  if (metadataIndexTried)
    return metadataIndexed ? 0 : 1;

  // Fast path: disk cache hit.
  if (TryLoadMetadataFromCache())
    return 0;

  // Cache miss: parse the source file.
  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  struct stat st;
  long long fsize = 0, fmtime = 0;
  if (stat(path.c_str(), &st) == 0) {
    fsize  = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }

  metadataIndexTried = true;
  int err = book_parser::IndexMetadata(this, path.c_str());

  if (err == BOOK_ERR_CANCELLED) {
    metadataIndexTried = false;
    metadataIndexed    = false;
    return err;
  }

  if (!err) {
    metadataIndexed = true;
    book_meta_cache::MetaEntry entry;
    entry.title            = title;
    entry.author           = author;
    entry.cover_image_path = coverImagePath;
    book_meta_cache::Save(
        book_meta_cache::BuildPath(path, fsize, fmtime), entry);
  }

  return err;
}
