/*
    3dslibris - epub_page_cache.h
    EPUB persistent page cache serialization.
    Extracted from epub.cpp by Rigle.
*/

#pragma once

#include <3ds.h>
#include <stdio.h>
#include <string>

class Book;

namespace epub_page_cache {

bool TryLoad(Book *book, const char *book_path, int pixel_size,
             int line_spacing, int paragraph_spacing, int paragraph_indent,
             int orientation, int margin_left, int margin_right, int margin_top,
             int margin_bottom, const char *regular_font);

void Save(Book *book, const char *book_path, int pixel_size,
          int line_spacing, int paragraph_spacing, int paragraph_indent,
          int orientation, int margin_left, int margin_right, int margin_top,
          int margin_bottom, const char *regular_font, bool closing = false);

void SavePending(Book *book, bool closing = false);

class StreamWriter {
public:
  StreamWriter();
  ~StreamWriter();

  bool Begin(Book *book, const char *book_path, int pixel_size, int line_spacing,
             int paragraph_spacing, int paragraph_indent, int orientation,
             int margin_left, int margin_right, int margin_top, int margin_bottom,
             const char *regular_font);

  bool FlushPages(Book *book, u16 from_page);

  bool Finalize(Book *book);

  void Abort();

  bool IsOpen() const { return fp_ != NULL; }

private:
  FILE *fp_;
  std::string cache_path_;
  u32 pages_written_;
};

} // namespace epub_page_cache
