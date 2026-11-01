#pragma once
#include "book/book.h"
#include "book/page.h"
#include "book/text_search.h"

namespace text_search {
inline bool ReadBookPage(void *context, uint32_t index, PageView *out) {
  Book *book = static_cast<Book *>(context);
  if (!book || index >= book->GetPageCount()) return false;
  Page *page = book->GetPage(index);
  if (!page || page->GetLength() < 0) return false;
  *out = {page->GetBuffer(), (size_t)page->GetLength(), &page->GetSearchEvents()};
  return true;
}
inline PageAvailability PrepareBookPage(void *context,uint32_t index) {
  Book *book=static_cast<Book*>(context);
  if(!book || index>=book->GetPageCount())return PageError;
  if(book->IsEpubPageReady(index))return PageReady;
  return book->RequestEpubPage(index)?PagePending:PageError;
}
inline Source FromBook(Book *book) {
  Source source = {book, book ? (uint32_t)book->GetPageCount() : 0, ReadBookPage,
      book&&book->HasEpubPageCache()?PrepareBookPage:nullptr};
  return source;
}
} // namespace text_search
