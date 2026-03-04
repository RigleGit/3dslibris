#include "bookmark_menu.h"

#include <3ds.h>
#include <string>
#include <vector>

#include "app.h"
#include "book.h"
#include "button.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

BookmarkMenu::BookmarkMenu(App *_app) : Menu(_app) { pagesize = 8; }

void BookmarkMenu::Init() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();
  book_pages.clear();

  if (!app || !app->bookcurrent)
    return;

  std::list<u16> *bmarks = app->bookcurrent->GetBookmarks();
  int idx = 0;
  for (auto pg : *bmarks) {
    book_pages.push_back(pg);

    Button *b = new Button(app->ts);
    b->Init();
    b->Move(0, (idx % pagesize) * b->GetHeight());

    char label[64];
    sprintf(label, "Page %d", pg + 1);
    b->SetLabel1(std::string(label));
    buttons.push_back(b);
    idx++;
  }

  selected = 0;
  page = 0;
  dirty = true;
}

BookmarkMenu::~BookmarkMenu() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();
}

void BookmarkMenu::Draw() {
  app->ts->SetScreen(app->ts->screenright);
  app->ts->SetColorMode(0); // Normal mode for menu
  app->ts->ClearScreen();

  // Draw header
  app->ts->SetPen(app->ts->margin.left, 5);
  app->ts->PrintString("Bookmarks");

  // Draw buttons
  u8 start = page * pagesize;
  u8 end = MIN(start + pagesize, buttons.size());

  for (int i = start; i < end; i++) {
    buttons[i]->Draw(i == selected);
  }

  // Draw footer
  char label[32];
  sprintf(label, "Pg %d/%d", GetCurrentPage(), GetPageCount());
  app->ts->SetPen(180, 290);
  app->ts->PrintString(label);

  dirty = false;
}

void BookmarkMenu::HandleInput(u16 keys) {
  auto key = app->key;

  if (keys & (KEY_A | key.down | key.r)) {
    handleButtonPress();
  } else if (keys & KEY_TOUCH) {
    handleTouchInput();
  } else if (keys & (KEY_UP | key.l | KEY_B | KEY_START | KEY_SELECT)) {
    // Go back to the book
    app->mode = APP_MODE_BOOK;
    if (app->bookcurrent) {
      app->bookcurrent->GetPage()->Draw(app->ts);
    }
    app->ts->SetScreen(app->ts->screenleft);
    app->ts->PrintSplash(app->ts->screenright);
  } else if (keys & key.right) {
    // next item
    selectNext();
  } else if (keys & key.left) {
    // prev item
    selectPrevious();
  } else if (keys & KEY_R) {
    nextPage();
  } else if (keys & KEY_L) {
    previousPage();
  }
}

void BookmarkMenu::selectNext() {
  if (selected < buttons.size() - 1) {
    selected++;
    page = selected / pagesize;
    dirty = true;
  }
}

void BookmarkMenu::selectPrevious() {
  if (selected > 0) {
    selected--;
    page = selected / pagesize;
    dirty = true;
  }
}

void BookmarkMenu::nextPage() {
  if (page < GetPageCount() - 1) {
    page++;
    selected = page * pagesize;
    dirty = true;
  }
}

void BookmarkMenu::previousPage() {
  if (page > 0) {
    page--;
    selected = page * pagesize;
    dirty = true;
  }
}

void BookmarkMenu::handleButtonPress() {
  if (buttons.empty())
    return;

  u16 targetPage = book_pages[selected];
  app->bookcurrent->SetPosition(targetPage);
  app->mode = APP_MODE_BOOK;
  app->bookcurrent->GetPage()->Draw(app->ts);
  app->ts->SetScreen(app->ts->screenleft);
  app->ts->PrintSplash(app->ts->screenright);
}

void BookmarkMenu::handleTouchInput() {
  touchPosition touch = app->TouchRead();

  u8 start = page * pagesize;
  u8 end = MIN(start + pagesize, buttons.size());

  for (int i = start; i < end; i++) {
    if (buttons[i]->EnclosesPoint(touch.px, touch.py)) {
      selected = i;
      handleButtonPress();
      return;
    }
  }
}
