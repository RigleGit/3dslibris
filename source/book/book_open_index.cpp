/*
    3dslibris - book_open_index.cpp
*/

#include "shared/path_utils.h"
#include "book/book.h"

#include <sys/stat.h>

#include "formats/common/book_meta_cache.h"

// Tries to populate title/author/coverImagePath from the disk cache without
// opening the source file. Returns true on cache hit.
bool Book::TryLoadMetadataFromCache(book_meta_cache::Index *index) {
  if (metadataIndexTried)
    return metadataIndexed;

  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  book_meta_cache::EnsureDir();
  struct stat st;
  long long fsize = 0, fmtime = 0;
  if (paths::StatWithMtime(path.c_str(), &st)) {
    fsize  = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }
  book_meta_cache::MetaEntry cached;
  const std::string cache_path = book_meta_cache::BuildPath(path, fsize, fmtime);
  if (!(index ? index->Load(cache_path, &cached)
              : book_meta_cache::Load(cache_path, &cached)))
    return false;

  if (!cached.title.empty())
    SetTitle(cached.title.c_str());
  if (!cached.author.empty())
    SetAuthor(cached.author);
  SetSeries(cached.series);
  SetLanguage(cached.language);
  SetPublisher(cached.publisher);
  SetPublished(cached.published);
  SetSubjects(cached.subjects);
  SetDescription(cached.description);
  coverImagePath     = cached.cover_image_path;
  metadataIndexTried = true;
  metadataIndexed    = true;
  ClearBrowserDisplayNameCache();
  return true;
}

