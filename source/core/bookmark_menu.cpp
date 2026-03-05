#include "bookmark_menu.h"

#include <stdio.h>

#include "app.h"
#include "book.h"

BookmarkMenu::BookmarkMenu(App *_app) : PagedListMenu(_app, "bookmarks") {}

BookmarkMenu::~BookmarkMenu() {}

void BookmarkMenu::BuildEntries(std::vector<std::string> &labels,
                                std::vector<u16> &pages) {
  if (!app || !app->bookcurrent)
    return;

  std::list<u16> *bookmarks = app->bookcurrent->GetBookmarks();
  labels.reserve(bookmarks->size());
  pages.reserve(bookmarks->size());

  for (auto pg : *bookmarks) {
    char label[64];
    sprintf(label, "Page %d", pg + 1);
    labels.push_back(std::string(label));
    pages.push_back(pg);
  }
}

