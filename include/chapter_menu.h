#pragma once

#include "menu.h"
#include <3ds.h>
#include <string>
#include <vector>

class ChapterMenu : public Menu {
public:
  ChapterMenu(class App *app);
  ~ChapterMenu();

  void Init();
  void Draw() override;
  void HandleInput(u32 keys) override;

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

  std::vector<u16> chapter_pages;
};

