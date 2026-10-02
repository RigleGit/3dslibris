#pragma once
#include "book/text_search.h"
#include "ui/button.h"
class Book;

// Owned solely by ReaderController. No pointers into Page storage survive a tick.
struct ReaderSearchSession {
  text_search::Engine engine;
  text_search::Match match{};
  Book *book = nullptr;
  unsigned session = 0, revision = 0, pages = 0;
  bool active = false, editor = true, whole_words = false, has_match = false;
  bool dirty = true, wait_release = true, keyboard_pending = false;
  int selected = 0;
  uint64_t release_ms = 0, started_ms = 0, scan_ms = 0, max_slice_ms = 0;
  std::string query, message;
  Button buttons[4];
};
