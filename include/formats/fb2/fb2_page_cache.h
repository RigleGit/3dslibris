#pragma once
#include <string>
#include "formats/common/page_cache_utils.h"
class Book;
namespace fb2_page_cache {
std::string CachePath(Book *, const char *,
                      const page_cache_utils::PageCacheLayoutParams &);
#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *);
#endif
} // namespace fb2_page_cache
