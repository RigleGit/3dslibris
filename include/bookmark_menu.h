#pragma once

#include "button.h"
#include "menu.h"
#include "text.h"
#include <3ds.h>
#include <string>
#include <vector>

class BookmarkMenu : public Menu {
public:
  BookmarkMenu(class App *app);
  ~BookmarkMenu();

  void Init();
  void Draw() override;
  void HandleInput(u16 keys) override;

  inline bool IsDirty() const { return dirty; }
  inline void SetDirty(bool d = true) { dirty = d; }

private:
  void handleButtonPress();
  void handleTouchInput();
  void returnToBook();
  void nextPage();
  void previousPage();
  void selectNext();
  void selectPrevious();

  std::vector<u16> book_pages;
};
