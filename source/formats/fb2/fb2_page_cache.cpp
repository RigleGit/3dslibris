#include "formats/fb2/fb2_page_cache.h"
#include "book/book.h"
#include "shared/path_constants.h"
namespace fb2_page_cache {
namespace {
#ifdef DSLIBRIS_HOST_TEST
std::string directory;
#endif
}
std::string CachePath(Book *book,const char *source,const page_cache_utils::PageCacheLayoutParams &params) {
  auto p=params;
  if(book && !book->GetEbookImagesEnabled()) p.variant_token="no-images";
#ifdef DSLIBRIS_HOST_TEST
  const std::string &dir=directory.empty() ? paths::GetFb2CacheDir() : directory;
#else
  const std::string &dir=paths::GetFb2CacheDir();
#endif
  return page_cache_utils::BuildPageCachePath(dir,".fpc",source,p);
}
#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *path){directory=path ? path : "";}
#endif
}
