#pragma once

#include "paged_list_menu.h"
#include <3ds.h>
#include <string>
#include <vector>

class ChapterMenu : public PagedListMenu {
public:
  ChapterMenu(class App *app);
  ~ChapterMenu();

private:
  void BuildEntries(std::vector<std::string> &labels,
                    std::vector<u16> &pages) override;
  bool ResolveTargetPage(u8 index, u16 *page_out) override;

  std::vector<std::string> entry_titles;
  std::vector<u16> entry_pages;
  std::vector<int> approx_page_cache;
};
