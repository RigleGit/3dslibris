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
          int margin_bottom, const char *regular_font,
          bool closing = false);

void SavePending(Book *book, bool closing = false);

class StreamWriter {
public:
  enum StepResult { Pending, Complete, Failed };
  StreamWriter();
  ~StreamWriter();
  StreamWriter(const StreamWriter &) = delete;
  StreamWriter &operator=(const StreamWriter &) = delete;

  bool Begin(Book *book, const char *book_path, int pixel_size, int line_spacing,
             int paragraph_spacing, int paragraph_indent, int orientation,
             int margin_left, int margin_right, int margin_top, int margin_bottom,
             const char *regular_font);

  bool FlushPages(Book *book, u16 from_page, bool closing = false);

  bool Finalize(Book *book, bool closing = false);
  StepResult Step(Book *book, size_t max_bytes, u32 max_ms,
                  bool closing = false);

  void Abort();

  bool IsOpen() const;

private:
  struct State;
  State *state_;
};

#ifdef DSLIBRIS_HOST_TEST
void SetCacheDirForTest(const char *dir);
#endif

} // namespace epub_page_cache
