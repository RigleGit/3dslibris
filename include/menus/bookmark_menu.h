/*
    3dslibris - bookmark_menu.h
    New 3DS menu module by Rigle.

    Summary:
    - Concrete paged list menu for user bookmarks in current book.
    - Builds touch/keyboard selectable entries from persisted bookmark pages.
*/

#pragma once

#include "ui/button.h"
#include "menus/paged_list_menu.h"
#include "ui/text.h"
#include <3ds.h>
#include <string>
#include <vector>

class BookmarkMenu : public PagedListMenu {
public:
  BookmarkMenu(class App *app);
  ~BookmarkMenu();

private:
  void BuildEntries(std::vector<std::string> &labels,
                    std::vector<u16> &pages) override;
};
