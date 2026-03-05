/*
    3dslibris - app_browser.cpp
    Adapted from dslibris for Nintendo 3DS.
*/

#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <3ds.h>

#include "book.h"
#include "button.h"
#include "epub.h"
#include "main.h"
#include "parse.h"
#include "text.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

void App::browser_handleevent() {
  u32 keys = hidKeysDown();

  if (keys & (KEY_A | key.down)) {
    // Open selected book.
    OpenBook();
  }

  else if (keys & (key.left | key.l)) {
    // Select next book.
    int b = GetBookIndex(bookselected);
    if (b < bookcount - 1) {
      b++;
      bookselected = books[b];
      if (b >= browserstart + APP_BROWSER_BUTTON_COUNT)
        browser_nextpage();
      browser_view_dirty = true;
    }
  }

  else if (keys & (key.right | key.r)) {
    // Select previous book.
    int b = GetBookIndex(bookselected);
    if (b > 0) {
      b--;
      bookselected = books[b];
      if (b < browserstart)
        browser_prevpage();
      browser_view_dirty = true;
    }
  }

  else if (keys & (KEY_SELECT | KEY_Y)) {
    ShowSettingsView(false);
  }

  else if (keys & KEY_TOUCH) {
    touchPosition coord = TouchRead();
    // TouchRead() now maps to buffer coords: coord.px=sx (origin.x),
    // coord.py=sy (origin.y)
    int tx = coord.px;
    int ty = coord.py;

    if (buttonnext.EnclosesPoint(tx, ty)) {
      browser_nextpage();
    } else if (buttonprev.EnclosesPoint(tx, ty)) {
      browser_prevpage();
    } else if (buttonprefs.EnclosesPoint(tx, ty)) {
      ShowSettingsView(false);
    } else {
      // Check if a book cover was tapped
      for (int i = browserstart;
           (i < bookcount) && (i < browserstart + APP_BROWSER_BUTTON_COUNT);
           i++) {
        if (buttons[i]->EnclosesPoint(tx, ty)) {
          if (bookselected == books[i]) {
            // Already selected, so open it
            OpenBook();
          } else {
            // First tap, select it
            bookselected = books[i];
            browser_view_dirty = true;
          }
          break;
        }
      }
    }
  }
}

// Grid layout constants for portrait covers
#define GRID_COLS 2
#define GRID_ROWS 2
#define COVER_W 85
#define COVER_H 115
#define CELL_W 115
#define CELL_H 140
#define GRID_X0 5
#define GRID_Y0 3

void App::browser_init(void) {
  for (int i = 0; i < bookcount; i++) {
    Book *book = books[i];
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % GRID_COLS;
    int row = page_idx / GRID_COLS;

    buttons.push_back(new Button());
    buttons[i]->Init(ts);
    // Button is the cover area - portrait orientation
    buttons[i]->Resize(COVER_W + 4, COVER_H + 4);
    buttons[i]->Move(GRID_X0 + col * CELL_W, GRID_Y0 + row * CELL_H);

    // Cover extraction moved to browser_draw to avoid freezing at startup

    // Set text label only for books without covers
    if (!book->coverPixels) {
      const char *title = books[i]->GetTitle();
      if (title && strlen(title))
        buttons[i]->SetLabel1(std::string(title));
      else
        buttons[i]->SetLabel1(std::string(books[i]->GetFileName()));
    }
  }

  buttonprev.Init(ts);
  buttonprev.Move(2, 300);
  buttonprev.Resize(50, 16);
  buttonprev.Label("prev");
  buttonnext.Init(ts);
  buttonnext.Move(188, 300);
  buttonnext.Resize(50, 16);
  buttonnext.Label("next");
  buttonprefs.Init(ts);
  buttonprefs.Move(80, 300);
  buttonprefs.Resize(78, 16);
  buttonprefs.Label("settings");

  if (!bookselected) {
    browserstart = 0;
    bookselected = books[0];
  } else {
    browserstart = (GetBookIndex(bookselected) / APP_BROWSER_BUTTON_COUNT) *
                   APP_BROWSER_BUTTON_COUNT;
  }
}

void App::browser_nextpage() {
  if (browserstart + APP_BROWSER_BUTTON_COUNT < bookcount) {
    browserstart += APP_BROWSER_BUTTON_COUNT;
    bookselected = books[browserstart];
    browser_view_dirty = true;
  }
}

void App::browser_prevpage() {
  if (browserstart - APP_BROWSER_BUTTON_COUNT >= 0) {
    browserstart -= APP_BROWSER_BUTTON_COUNT;
    bookselected = books[browserstart + APP_BROWSER_BUTTON_COUNT - 1];
    browser_view_dirty = true;
  }
}

void App::browser_draw(void) {
  // save state
  int colorMode = ts->GetColorMode();
  u16 *screen = ts->GetScreen();
  int style = ts->GetStyle();
  int savedPixelSize = ts->pixelsize;

  ts->SetScreen(ts->screenright);
  ts->SetColorMode(0); // Normal for browser text
  ts->ClearScreen();

  // Metadata/cover work only for the selected book to avoid startup stalls.
  if (bookselected && bookselected->format == FORMAT_EPUB &&
      !bookselected->metadataIndexTried) {
    bookselected->Index();
    browser_view_dirty = true;
  }

  for (int i = browserstart;
       (i < bookcount) && (i < browserstart + APP_BROWSER_BUTTON_COUNT); i++) {
    buttons[i]->Draw(ts->screenright, books[i] == bookselected);

    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % GRID_COLS;
    int row = page_idx / GRID_COLS;
    int btnX = GRID_X0 + col * CELL_W;
    int btnY = GRID_Y0 + row * CELL_H;

    if (books[i] == bookselected && !books[i]->coverPixels &&
        !books[i]->coverTried && books[i]->format == FORMAT_EPUB) {
      if (books[i]->metadataIndexTried) {
        if (!books[i]->coverImagePath.empty()) {
          std::string path = bookdir + "/" + books[i]->GetFileName();
          epub_extract_cover(books[i], path);
        }
        books[i]->coverTried = true;
      }
    }

    if (books[i]->coverPixels) {
      int cx = btnX + 2 + (COVER_W - books[i]->coverWidth) / 2;
      int cy = btnY + 2 + (COVER_H - books[i]->coverHeight) / 2;
      int w = ts->display.height; // buffer stride
      for (int py = 0; py < books[i]->coverHeight && (cy + py) < 320; py++) {
        for (int px = 0; px < books[i]->coverWidth && (cx + px) < 240; px++) {
          ts->screenright[(cy + py) * w + (cx + px)] =
              books[i]->coverPixels[py * books[i]->coverWidth + px];
        }
      }
    }

    if (books[i] == bookselected) {
      ts->DrawRect(btnX - 2, btnY - 2, btnX + CELL_W + 2, btnY + CELL_H + 2,
                   0xF800); // Red thick outer bounding box
      ts->DrawRect(btnX - 3, btnY - 3, btnX + CELL_W + 3, btnY + CELL_H + 3,
                   0xF800);
      ts->SetStyle(TEXT_STYLE_BOLD);
    } else {
      ts->SetStyle(TEXT_STYLE_REGULAR);
    }

    // Draw title below the cover in small font
    ts->SetPixelSize(10);
    const char *title = books[i]->GetTitle();
    if (title && strlen(title)) {
      char truncTitle[20];
      strncpy(truncTitle, title, 19);
      truncTitle[19] = '\0';
      ts->SetPen(btnX, btnY + COVER_H + 12);
      ts->PrintString(truncTitle);
    }

    // Draw progress indicator
    int pos = books[i]->GetPosition();
    char msg[16];
    if (pos > 0)
      sprintf(msg, "Pg %d", pos + 1);
    else
      sprintf(msg, "NEW");
    ts->SetPen(btnX, btnY + COVER_H + 24);
    ts->PrintString(msg);
  }

  ts->SetPixelSize(savedPixelSize);

  // Navigation buttons at the bottom
  if (browserstart >= APP_BROWSER_BUTTON_COUNT)
    buttonprev.Draw(ts->screenright, false);
  if (bookcount > browserstart + APP_BROWSER_BUTTON_COUNT)
    buttonnext.Draw(ts->screenright, false);

  buttonprefs.Draw(ts->screenright, false);

  // Pagination indicator
  if (bookcount > APP_BROWSER_BUTTON_COUNT) {
    int currentPage = (browserstart / APP_BROWSER_BUTTON_COUNT) + 1;
    int totalPages =
        (bookcount + APP_BROWSER_BUTTON_COUNT - 1) / APP_BROWSER_BUTTON_COUNT;
    char pageMsg[16];
    sprintf(pageMsg, "%d/%d", currentPage, totalPages);
    ts->SetPixelSize(8);
    ts->SetPen(112, 303);
    ts->PrintString(pageMsg);
    ts->SetPixelSize(savedPixelSize);
  }

  bool pendingLazyWork = false;
  if (bookselected && bookselected->format == FORMAT_EPUB &&
      (!bookselected->metadataIndexTried || !bookselected->coverTried)) {
    pendingLazyWork = true;
  }

  // restore state
  ts->SetColorMode(colorMode);
  ts->SetScreen(screen);
  ts->SetStyle(style);

  browser_view_dirty = pendingLazyWork;
}
