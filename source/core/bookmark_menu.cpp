#include "bookmark_menu.h"

#include <3ds.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "app.h"
#include "book.h"
#include "button.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

static const int BOOKMARK_HEADER_X = 6;
static const int BOOKMARK_HEADER_Y = 14;
static const int BOOKMARK_ROW_X = 5;
static const int BOOKMARK_ROW_Y0 = 24;
static const int BOOKMARK_ROW_W = 230;
static const int BOOKMARK_ROW_H = 34;
static const int BOOKMARK_ROW_GAP = 2;
static const int BOOKMARK_FOOTER_Y = 292;

static void LayoutFooterButtons(App *app) {
  app->buttonprev.Move(6, BOOKMARK_FOOTER_Y);
  app->buttonprev.Resize(68, 22);
  app->buttonprev.Label("prev");

  app->buttonprefs.Move(86, BOOKMARK_FOOTER_Y);
  app->buttonprefs.Resize(68, 22);
  app->buttonprefs.Label("back");

  app->buttonnext.Move(166, BOOKMARK_FOOTER_Y);
  app->buttonnext.Resize(68, 22);
  app->buttonnext.Label("next");
}

BookmarkMenu::BookmarkMenu(App *_app) : Menu(_app) { pagesize = 7; }

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
    b->Resize(BOOKMARK_ROW_W, BOOKMARK_ROW_H);
    b->Move(BOOKMARK_ROW_X,
            BOOKMARK_ROW_Y0 + (idx % pagesize) * (BOOKMARK_ROW_H + BOOKMARK_ROW_GAP));

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
  app->ts->SetPen(BOOKMARK_HEADER_X, BOOKMARK_HEADER_Y);
  app->ts->PrintString("bookmarks");

  // Draw buttons
  u8 start = page * pagesize;
  u8 end = MIN(start + pagesize, buttons.size());

  for (int i = start; i < end; i++) {
    buttons[i]->Draw(i == selected);
  }

  // Draw footer
  char label[32];
  sprintf(label, "Pg %d/%d", GetCurrentPage(), GetPageCount());
  app->ts->SetPen(6, 282);
  app->ts->PrintString(label);

  LayoutFooterButtons(app);
  if (page > 0) {
    app->buttonprev.Draw();
  }
  app->buttonprefs.Draw();
  if (page < GetPageCount() - 1) {
    app->buttonnext.Draw();
  }

  dirty = false;
}

void BookmarkMenu::HandleInput(u32 keys) {
  auto key = app->key;
  if (keys & KEY_TOUCH) {
    handleTouchInput();
  } else if (keys & KEY_A) {
    handleButtonPress();
  } else if (keys & (KEY_B | KEY_START | KEY_SELECT)) {
    returnToBook();
  } else if (keys & (key.down | KEY_DOWN | key.right | KEY_RIGHT |
                     KEY_CPAD_DOWN | KEY_CPAD_RIGHT)) {
    // next item
    selectNext();
  } else if (keys & (key.up | KEY_UP | key.left | KEY_LEFT | KEY_CPAD_UP |
                     KEY_CPAD_LEFT)) {
    // prev item
    selectPrevious();
  } else if (keys & (key.r | KEY_R)) {
    nextPage();
  } else if (keys & (key.l | KEY_L)) {
    previousPage();
  }
}

void BookmarkMenu::selectNext() {
  if (buttons.empty())
    return;
  if ((size_t)selected + 1 < buttons.size()) {
    selected++;
    page = selected / pagesize;
    dirty = true;
  }
}

void BookmarkMenu::selectPrevious() {
  if (buttons.empty())
    return;
  if (selected > 0) {
    selected--;
    page = selected / pagesize;
    dirty = true;
  }
}

void BookmarkMenu::nextPage() {
  if (buttons.empty())
    return;
  if (page < GetPageCount() - 1) {
    page++;
    u8 next_selected = page * pagesize;
    selected = (next_selected < buttons.size()) ? next_selected
                                                : (u8)(buttons.size() - 1);
    dirty = true;
  }
}

void BookmarkMenu::previousPage() {
  if (buttons.empty())
    return;
  if (page > 0) {
    page--;
    selected = page * pagesize;
    dirty = true;
  }
}

void BookmarkMenu::handleButtonPress() {
  if (buttons.empty() || selected >= book_pages.size() || !app->bookcurrent)
    return;

  u16 targetPage = book_pages[selected];
  app->bookcurrent->SetPosition(targetPage);
  returnToBook();
}

void BookmarkMenu::handleTouchInput() {
  LayoutFooterButtons(app);
  touchPosition touch = app->TouchRead();
  touchPosition raw;
  hidTouchRead(&raw);

  const int candidates[4][2] = {
      {(int)touch.px, (int)touch.py},
      {(int)raw.py, (int)raw.px},
      {239 - (int)raw.py, (int)raw.px},
      {(int)raw.py, 319 - (int)raw.px},
  };

  // Robust fallback zones for footer buttons.
  int footerX = -1;
  for (int i = 0; i < 4; i++) {
    int x = candidates[i][0];
    int y = candidates[i][1];
    if (x < 0 || y < 0)
      continue;
    if (x > 239 || y > 319)
      continue;
    if (y >= 284) {
      footerX = x;
      break;
    }
  }
  if (footerX >= 0) {
    if (footerX < 80) {
      previousPage();
    } else if (footerX < 160) {
      returnToBook();
    } else {
      nextPage();
    }
    return;
  }

  // Coarse row hit-test fallback: robust even if button hitboxes drift.
  for (int i = 0; i < 4; i++) {
    int x = candidates[i][0];
    int y = candidates[i][1];
    if (x < BOOKMARK_ROW_X || x >= BOOKMARK_ROW_X + BOOKMARK_ROW_W)
      continue;
    if (y < BOOKMARK_ROW_Y0)
      continue;
    int row = (y - BOOKMARK_ROW_Y0) / (BOOKMARK_ROW_H + BOOKMARK_ROW_GAP);
    if (row < 0 || row >= (int)pagesize)
      continue;
    int row_y = BOOKMARK_ROW_Y0 + row * (BOOKMARK_ROW_H + BOOKMARK_ROW_GAP);
    if (y >= row_y + BOOKMARK_ROW_H)
      continue;
    int idx = (int)(page * pagesize) + row;
    if (idx < 0 || idx >= (int)buttons.size())
      continue;
    selected = (u8)idx;
    handleButtonPress();
    return;
  }

  auto enclosesWithSlack = [&](Button &button, int x, int y) {
    for (int dy = -4; dy <= 4; dy += 4) {
      for (int dx = -4; dx <= 4; dx += 4) {
        int tx = x + dx;
        int ty = y + dy;
        if (tx < 0 || ty < 0)
          continue;
        if (button.EnclosesPoint((u16)tx, (u16)ty))
          return true;
      }
    }
    return false;
  };

  auto hitsButton = [&](Button &button) {
    for (int i = 0; i < 4; i++) {
      int x = candidates[i][0];
      int y = candidates[i][1];
      if (x < 0 || y < 0)
        continue;
      if (x > 239 || y > 319)
        continue;
      if (enclosesWithSlack(button, x, y))
        return true;
    }
    return false;
  };

  if (hitsButton(app->buttonprefs)) {
    returnToBook();
    return;
  }
  if (hitsButton(app->buttonnext)) {
    nextPage();
    return;
  }
  if (hitsButton(app->buttonprev)) {
    previousPage();
    return;
  }

  u8 start = page * pagesize;
  u8 end = MIN(start + pagesize, buttons.size());

  for (int i = start; i < end; i++) {
    if (hitsButton(*buttons[i])) {
      selected = i;
      handleButtonPress();
      return;
    }
  }
}

void BookmarkMenu::returnToBook() {
  app->mode = APP_MODE_BOOK;
  if (app->bookcurrent) {
    app->bookcurrent->GetPage()->Draw(app->ts);
  }
  app->ts->SetScreen(app->ts->screenleft);
  app->ts->PrintSplash(app->ts->screenright);
}
