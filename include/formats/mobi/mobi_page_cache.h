#pragma once

#include <3ds.h>
#include <string>
#include "formats/common/page_cache_utils.h"
#include "formats/common/reflow_block_cache.h"

class Book;

namespace mobi_page_cache {
std::string CachePath(Book *book,const char *source,const page_cache_utils::PageCacheLayoutParams &params);
class LegacyLoader {
public:
  LegacyLoader();
  ~LegacyLoader();
  reflow_block_cache::Result Begin(Book *book,const std::string &path);
  reflow_block_cache::Result Step();
  unsigned Progress() const;
  unsigned Total() const;
private:
  struct State;
  State *state_;
  LegacyLoader(const LegacyLoader &);
  LegacyLoader &operator=(const LegacyLoader &);
};


bool TryLoad(Book *book, const char *book_path,
             int pixel_size, int line_spacing,
             int paragraph_spacing, int paragraph_indent,
             int orientation, int margin_left, int margin_right,
             int margin_top, int margin_bottom,
             const char *regular_font,
             bool line_wrap_fix_enabled);

void Save(Book *book, const char *book_path,
          int pixel_size, int line_spacing,
          int paragraph_spacing, int paragraph_indent,
          int orientation, int margin_left, int margin_right,
          int margin_top, int margin_bottom,
          const char *regular_font,
          bool line_wrap_fix_enabled);

void SavePending(Book *book);

#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *dir);
void SetMaxBulkCacheLoadBytesForTest(long bytes);
#endif

} // namespace mobi_page_cache
