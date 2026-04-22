/*
    3dslibris - menu.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Abstract menu base class shared by browser/settings/index/bookmark UIs.
    - Provides common button collection, pagination and selection state.
*/

#pragma once
/*  Abstract base for Book, Pref and Font menus. */
#include <3ds.h>
#include <vector>

class Menu {
public:
  Menu(class App *app);
  virtual ~Menu();
  virtual void Draw() = 0; // Draw the menu on the screen
  virtual u16 GetCurrentPage() const;
  virtual u16 GetPageCount() const;
  virtual void HandleInput(u32 keys) = 0; // Handle input events
  virtual void SelectItem(u16 index);

  class App *app; //! Pointer to the application instance.
  std::vector<class Button *> buttons;
  bool dirty;
  u16 page;
  u16 pagesize;
  u16 selected;
};
