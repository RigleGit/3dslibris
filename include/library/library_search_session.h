#pragma once
#include "library/library_search.h"
#include "ui/button.h"

struct LibrarySearchSession {
  library_search::Engine engine;
  library_search::Field field=library_search::Field::Name;
  std::string query,message,origin_folder,origin_selection;
  int origin_page=0,selected=0;
  size_t result_selection=0;
  bool editor=true,dirty=true,wait_release=true;
  Button editor_buttons[4],rows[5],footer[3];
  void NextField() {
    field=(library_search::Field)(((int)field+1)%3);
  }
};
