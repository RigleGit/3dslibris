#include "chapter_menu.h"

#include "app.h"
#include "book.h"

ChapterMenu::ChapterMenu(App *_app) : PagedListMenu(_app, "index") {}

ChapterMenu::~ChapterMenu() {}

void ChapterMenu::BuildEntries(std::vector<std::string> &labels,
                               std::vector<u16> &pages) {
  if (!app || !app->bookcurrent)
    return;

  const std::vector<ChapterEntry> &chapters = app->bookcurrent->GetChapters();
  labels.reserve(chapters.size());
  pages.reserve(chapters.size());

  for (const auto &ch : chapters) {
    labels.push_back(ch.title);
    pages.push_back(ch.page);
  }
}

