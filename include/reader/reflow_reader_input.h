/*
    3dslibris - reflow_reader_input.h
    New 3DS reader module by Rigle.

    Summary:
    - Input handling for reflowable books (EPUB/FB2/MOBI/TXT etc.) in reading mode.
    - Extracted from reader/app_book.cpp HandleEventInBook reflowable branch.
*/

#pragma once

#include <stdint.h>

#include "reader/reader_controls.h"

class App;
class Book;
class Prefs;
class Text;

namespace reflow_input {

// Handles all reflowable input for one event frame.
// Returns true if the display needs a status redraw.
bool HandleInBook(App &app, Book *book, Text *ts, Prefs *prefs, uint32_t keys,
                  uint32_t held, uint16_t *pagecurrent, uint16_t *pagecount,
                  const ReaderControls &ctrl);

} // namespace reflow_input
