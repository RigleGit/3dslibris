#include "menu.h"
#include "app.h"

Menu::Menu(App *_app) {
    app = _app;
    buttons.clear();
    pagesize = 7;
    selected = 0;
    page = selected / pagesize; // Calculate the current page based on selected item
    dirty = true;
}

Menu::~Menu() {}

u8 Menu::GetCurrentPage() const {
    return selected / pagesize + 1; // Pages are 1-indexed
}

u8 Menu::GetPageCount() const {
    if (pagesize == 0 || buttons.empty()) {
        return 1;
    }
    return (buttons.size() + pagesize - 1) / pagesize;
}

void Menu::SelectItem(u8 index) {
    if (index >= buttons.size()) {
        return; // Invalid index
    }
    selected = index;
    page = selected / pagesize; // Update the current page based on the selected item
    dirty = true; // Mark the menu as dirty to redraw
}
