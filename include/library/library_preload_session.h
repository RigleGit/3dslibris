#pragma once
#include "library/library_preload.h"
#include "ui/button.h"
#include <memory>
class Book;

struct LibraryPreloadSession {
  library_preload::Engine engine;
  std::unique_ptr<Book> book;
  std::string book_path, message;
  bool metadata_failed = false;
  bool start_pending = true, finished = false, wait_release = true;
  Button action;
};
