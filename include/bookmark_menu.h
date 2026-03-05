#pragma once

#include "button.h"
#include "paged_list_menu.h"
#include "text.h"
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
