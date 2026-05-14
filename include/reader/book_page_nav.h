/*
    3dslibris - book_page_nav.h
    New 3DS reader module by Rigle.

    Summary:
    - Shared page-navigation primitives used by both fixed-layout and
      reflowable input handlers and by the book-open flow.
*/

#pragma once

#include <stdint.h>

class Book;
class Text;

namespace book_nav {

void DrawPage(Book *book, Text *ts);
bool SetPage(Book *book, Text *ts, uint16_t page);
bool TurnPage(Book *book, Text *ts, uint16_t *pagecurrent, uint16_t pagecount,
              int delta);
bool AdvancePage(Book *book, Text *ts, uint16_t *pagecurrent,
                 uint16_t *pagecount, bool *status_dirty);

} // namespace book_nav
