#pragma once

#include <3ds.h>
#include <string>

class Book;

namespace mobi_page_cache {

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

} // namespace mobi_page_cache
