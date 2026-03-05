#pragma once

#include "menu.h"
#include <3ds.h>
#include <string>
#include <vector>

class PagedListMenu : public Menu {
public:
  PagedListMenu(class App *app, const char *header_title);
  virtual ~PagedListMenu();

  void Init();
  void Draw() override;
  void HandleInput(u32 keys) override;

  inline bool IsDirty() const { return dirty; }
  inline void SetDirty(bool d = true) { dirty = d; }

protected:
  virtual void BuildEntries(std::vector<std::string> &labels,
                            std::vector<u16> &pages) = 0;

private:
  void ActivateSelected();
  void Back();
  void NextPage();
  void PreviousPage();
  void SelectNext();
  void SelectPrevious();
  void HandleTouchInput();
  void LayoutFooterButtons();

  std::string header_title;
  std::vector<u16> target_pages;
  bool wait_input_release;
};
