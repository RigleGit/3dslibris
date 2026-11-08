#include "book/book_parse_deps.h"
#include "formats/fb2/fb2_page_cache.h"
#include "book/book.h"
#include "shared/path_constants.h"
namespace fb2_page_cache {
namespace {
#ifdef DSLIBRIS_HOST_TEST
std::string directory;
#endif
} // namespace
std::string CachePath(Book *book, const char *source,
                      const page_cache_utils::PageCacheLayoutParams &params) {
  auto p = params;
  p.variant_token = "pub3:";
  p.variant_token +=
      book && book->GetPublisherTextIndentEnabled() ? "i1" : "i0";
  p.variant_token +=
      book && book->GetPublisherBlockMarginsEnabled() ? ":m1" : ":m0";
  p.variant_token +=
      book && book->GetPublisherHorizontalMarginsEnabled() ? ":h1" : ":h0";
  p.variant_token +=
      std::string(":a") + (char)('0' + (book ? book->GetTextAlignMode() : 0));
  if (book && !book->GetEbookImagesEnabled())
    p.variant_token += ":no-images";
  p.variant_token += CapturePageCacheFontIdentity(book ? book->GetText() : NULL);
#ifdef DSLIBRIS_HOST_TEST
  const std::string &dir =
      directory.empty() ? paths::GetFb2CacheDir() : directory;
#else
  const std::string &dir = paths::GetFb2CacheDir();
#endif
  return page_cache_utils::BuildPageCachePath(dir, ".fpc", source, p);
}
#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *path) {
  directory = path ? path : "";
}
#endif
} // namespace fb2_page_cache
