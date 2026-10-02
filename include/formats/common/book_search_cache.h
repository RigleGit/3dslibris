#pragma once

#include "book/book.h"
#include "book/page.h"
#include "formats/common/page_cache_utils.h"
#include <stdio.h>

namespace book_search_cache {
inline bool ReadRaw(FILE *fp, void *out, size_t size) {
  return fread(out, 1, size, fp) == size;
}
inline bool ReadRaw(page_cache_utils::BufReader *r, void *out, size_t size) {
  return r->ReadRaw(out, size);
}
template <typename Reader> bool Read(Reader *r, Book *book) {
  for (unsigned i = 0; i < book->GetPageCount(); ++i) {
    Page *page = book->GetPage(i);
    uint16_t count = 0;
    if (!ReadRaw(r, &count, sizeof(count)) || count > page->GetLength() + 1)
      return false;
    page_search::Events events(count);
    if (count && !ReadRaw(r, events.data(), count * sizeof(uint32_t)))
      return false;
    if (!page_search::Valid(events, page->GetLength())) return false;
    page->SetSearchEvents(events);
  }
  return true;
}
inline bool Write(FILE *fp, Book *book) {
  for (unsigned i = 0; i < book->GetPageCount(); ++i) {
    Page *page = book->GetPage(i);
    const page_search::Events &events = page->GetSearchEvents();
    if (events.size() > 65535 || !page_search::Valid(events, page->GetLength()))
      return false;
    uint16_t count = (uint16_t)events.size();
    if (fwrite(&count, 1, sizeof(count), fp) != sizeof(count)) return false;
    size_t bytes = count * sizeof(uint32_t);
    if (bytes && fwrite(events.data(), 1, bytes, fp) != bytes) return false;
  }
  return true;
}
} // namespace book_search_cache
