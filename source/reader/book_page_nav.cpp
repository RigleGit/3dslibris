/*
    3dslibris - book_page_nav.cpp
    New 3DS reader module by Rigle.

    Summary:
    - Shared page-navigation primitives extracted from reader/app_book.cpp.
    - Used by fixed-layout input, reflowable input, and the book-open flow.
*/

#include "reader/book_page_nav.h"

#include <3ds.h>

#include "book/book.h"
#include "book/book_renderer.h"
#include "ui/text.h"

namespace book_nav {

void DrawPage(Book *book, Text *ts) {
  if (!book || !ts || book->GetPageCount() == 0)
    return;
  book_renderer::DrawCurrentView(book, ts);
}

bool SetPage(Book *book, Text *ts, uint16_t page) {
  if (!book || !ts || page >= book->GetPageCount())
    return false;
  if (book->IsCbz())
    book->ResetCbzFailureState();
  if (book->IsFixedLayout())
    book_renderer::CancelFixedLayoutDeferredWork(book);
  book->SetPosition(page);
  book->ClearFocusedInlineLink();
  if (book->IsFixedLayout())
    book_renderer::ResetFixedLayoutViewportForNavigation(book);
  DrawPage(book, ts);
  return true;
}

bool TurnPage(Book *book, Text *ts, uint16_t *pagecurrent, uint16_t pagecount,
              int delta) {
  if (!book || !ts || !pagecurrent || pagecount == 0)
    return false;
  if (delta < 0) {
    if (*pagecurrent == 0)
      return false;
  } else if (*pagecurrent >= pagecount - 1) {
    return false;
  }
  *pagecurrent = (uint16_t)((int)*pagecurrent + delta);
  return SetPage(book, ts, *pagecurrent);
}

bool AdvancePage(Book *book, Text *ts, uint16_t *pagecurrent,
                 uint16_t *pagecount, bool *status_dirty) {
  if (!book || !ts || !pagecurrent || !pagecount || !status_dirty)
    return false;
  if (TurnPage(book, ts, pagecurrent, *pagecount, 1)) {
    *status_dirty = true;
    return true;
  }
  return false;
}

} // namespace book_nav
