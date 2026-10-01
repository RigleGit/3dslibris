#include "menus/menu.h"
#include "menus/menu_context.h"

#include <cstdio>
#include <cstdlib>

static void Expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

class TestMenu : public Menu {
public:
  explicit TestMenu(const MenuContext &context) : Menu(context) {}

  void Draw() override {}
  void HandleInput(const FrameInput &) override {}
};

int main() {
  const MenuContext empty = {};
  TestMenu menu(empty);
  Expect(menu.pagesize == 7 && menu.selected == 0 && menu.page == 0,
         "new menu starts on the first seven-item page");
  Expect(menu.dirty, "new menu needs drawing");
  Expect(menu.GetCurrentPage() == 1 && menu.GetPageCount() == 1,
         "empty menu displays page one of one");
  menu.dirty = false;
  menu.SelectItem(0);
  Expect(!menu.dirty && menu.selected == 0,
         "empty menu ignores a nonexistent item");

  const size_t sizes[] = {7, 8, 14, 15};
  const u16 counts[] = {1, 2, 2, 3};
  for (size_t i = 0; i < 4; ++i) {
    // Widgets are never dereferenced by the base menu's pagination API.
    menu.buttons.resize(sizes[i], nullptr);
    Expect(menu.GetPageCount() == counts[i],
           "partial final page counts as a full page");
  }
  const u16 selections[] = {6, 7, 14};
  const u16 pages[] = {1, 2, 3};
  for (size_t i = 0; i < 3; ++i) {
    menu.dirty = false;
    menu.SelectItem(selections[i]);
    Expect(menu.selected == selections[i] && menu.GetCurrentPage() == pages[i],
           "selection crosses the page boundary");
    Expect(menu.page == pages[i] - 1 && menu.dirty,
           "selection updates drawing page and requests a redraw");
  }
  menu.dirty = false;
  menu.SelectItem(15);
  menu.SelectItem(65535);
  Expect(menu.selected == 14 && menu.page == 2 && !menu.dirty,
         "invalid selections preserve the current view");
  menu.pagesize = 0;
  Expect(menu.GetPageCount() == 1, "disabled pagination reports one page");

  std::printf("All menu_context tests passed.\n");
  return 0;
}
