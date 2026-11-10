#pragma once
#include "library/library_search.h"
#include <vector>
#include "ui/button.h"

struct LibrarySearchSession {
  library_search::Engine engine;
  library_search::Field field=library_search::Field::Name;
  std::string query,message,origin_folder,origin_selection;
  int origin_page=0,selected=0;
  size_t result_selection=0;
  bool editor=true,dirty=true,wait_release=true;
  // Result rows are sized by their wrapped text, so a page holds a variable
  // number of them; `page_starts` lists the first result of every page.
  static const int kMaxRows=8;
  Button editor_buttons[4],rows[kMaxRows],footer[3];
  std::vector<int> row_heights,page_starts;
  size_t measured=0;
  void NextField() {
    field=(library_search::Field)(((int)field+1)%3);
  }
};
