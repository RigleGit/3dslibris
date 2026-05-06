/*
    3dslibris - paged_list_menu.h
    New 3DS menu module by Rigle.

    Summary:
    - Reusable paged list controller for chapter/bookmark/font pickers.
    - Provides button layout, page navigation and target-page activation hooks.
*/

#pragma once

#include "menus/menu.h"
#include <3ds.h>
#include <string>
#include <vector>

class Book;

class PagedListMenu : public Menu {
public:
  PagedListMenu(class App *app, const char *header_title);
  virtual ~PagedListMenu();

  void Init();
  void Draw() override;
  void HandleInput(u32 keys) override;
  u16 GetCurrentPage() const override;
  u16 GetPageCount() const override;
  void SelectItem(u16 index) override;

  inline bool IsDirty() const { return dirty; }
  inline void SetDirty(bool d = true) { dirty = d; }
  inline void DisableInitialReleaseWait() {
    wait_input_release = false;
    wait_input_release_started_ms = 0;
  }

protected:
  inline void SetHeaderTitle(const std::string &title) { header_title = title; }
  virtual void BuildEntries(Book *book, class Text *text,
                            std::vector<std::string> &labels,
                            std::vector<u16> &pages) = 0;
  virtual bool ResolveTargetPage(u16 index, u16 *page_out);

  Book *current_book_;

  u16 GetPageStart(u16 page_index) const;
  u16 GetPageSize(u16 page_index) const;
  u16 GetPageForIndex(u16 index) const;
  void UpdatePageSize();
  std::vector<u16> page_sizes;

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
  u64 wait_input_release_started_ms;
};
