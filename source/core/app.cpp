/*
    3dslibris - app.cpp
    Adapted from dslibris for Nintendo 3DS.

    NDS-specific hardware calls replaced with libctru equivalents or stubs.
*/

#include "app.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>

#include "book.h"
#include "bookmark_menu.h"
#include "button.h"
#include "font.h"
#include "main.h"
#include "parse.h"
#include "text.h"
#include "version.h"

App::App() {
  melonds = false;

  fontdir = std::string("sdmc:/3ds/3dslibris/font");
  bookdir = std::string("sdmc:/3ds/3dslibris/book");
  bookcount = 0;
  bookselected = NULL;
  bookcurrent = NULL;
  reopen = true;
  mode = APP_MODE_BROWSER;
  browserstart = 0;
  cache = false;
  orientation = false;
  paraspacing = 1;
  paraindent = 0;
  brightness = 1;
  colorMode = 0;

  key.down = KEY_DOWN;
  key.up = KEY_UP;
  key.left = KEY_LEFT;
  key.right = KEY_RIGHT;
  key.start = KEY_START;
  key.select = KEY_SELECT;
  key.l = KEY_L;
  key.r = KEY_R;
  key.a = KEY_A;
  key.b = KEY_B;
  key.x = KEY_X;
  key.y = KEY_Y;

  browser_view_dirty = false;

  prefs = new Prefs(this);
  prefsSelected = -1;
  prefs_view_dirty = false;
  prefs_book_context = false;

  ts = new Text();
  ts->app = this;

  fontmenu = new FontMenu(this);
  bookmarkmenu = new BookmarkMenu(this);
}

App::~App() {
  if (prefs)
    delete prefs;
  if (ts)
    delete ts;
  for (std::vector<Book *>::iterator it = books.begin(); it != books.end();
       it++)
    delete *it;
  books.clear();
  delete fontmenu;
  delete bookmarkmenu;
}

// std::sort comparator: books by title
static bool book_title_lessthan(Book *a, Book *b) {
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

int App::Run(void) {
  const int ok = 0;
  auto drawBootStatus = [&](const char *lineTop, const char *lineBottom) {
    int savedStyle = ts->GetStyle();
    int savedColorMode = ts->GetColorMode();
    u16 *savedScreen = ts->GetScreen();

    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->SetColorMode(0);

    ts->SetScreen(ts->screenleft);
    ts->ClearScreen();
    ts->SetPen(12, 28);
    ts->PrintString("3dslibris");
    if (lineTop && *lineTop) {
      ts->SetPen(12, 52);
      ts->PrintString(lineTop);
    }
    if (lineBottom && *lineBottom) {
      ts->SetPen(12, 72);
      ts->PrintString(lineBottom);
    }

    ts->SetScreen(ts->screenright);
    ts->ClearScreen();
    if (lineTop && *lineTop) {
      ts->SetPen(12, 28);
      ts->PrintString(lineTop);
    }
    if (lineBottom && *lineBottom) {
      ts->SetPen(12, 48);
      ts->PrintString(lineBottom);
    }

    ts->SetStyle(savedStyle);
    ts->SetColorMode(savedColorMode);
    ts->SetScreen(savedScreen);

    ts->BlitToFramebuffer();
    gfxFlushBuffers();
    gfxSwapBuffers();
  };

  // Start up typesetter.
  printf("Loading fonts...\n");
  if (ts->Init() != ok) {
    printf("\n[FAIL] Could not load fonts!\n");
    printf("Place TTF files in:\n");
    printf("  %s/\n\n", fontdir.c_str());
    printf("Required files:\n");
    printf("  LiberationSerif-Regular.ttf\n");
    printf("  LiberationSerif-Bold.ttf\n");
    printf("  LiberationSerif-Italic.ttf\n");
    printf("  LiberationSerif-BoldItalic.ttf\n");
    printf("  LiberationSans-Regular.ttf\n");
    return 1;
  }

  // Initialize screens for 3DS.
  InitScreens();
  ts->SetStyle(TEXT_STYLE_BROWSER);
  drawBootStatus("Searching for books...", "");

  // Construct library.
  PrintStatus("Searching for books...");
  if (FindBooks() != ok) {
    PrintStatus("error: no book directory");
    drawBootStatus("No se encontro carpeta de libros",
                   "Usa sdmc:/3ds/3dslibris/book");
    return 1;
  }
  if (bookcount == 0) {
    PrintStatus("error: no epub files found");
    drawBootStatus("No se encontraron EPUB", bookdir.c_str());
    return 1;
  }

  std::sort(books.begin(), books.end(), &book_title_lessthan);

  prefs->Read();
  drawBootStatus("Preparing library...", "");
  // Apply key mapping/orientation loaded from prefs.
  SetOrientation(orientation);
  PrintStatus("Preparing library...");
  for (auto &book : books) {
    book->GetBookmarks()->sort();
  }
  PrintStatus("Library ready.");

  // Set up menus.
  PrefsInit();
  browser_init();
  browser_view_dirty = true;

  PrintStatus(VERSION);

  // Resume reading from the last session.
  if (reopen && bookcurrent) {
    bookselected = bookcurrent;
    OpenBook();
  }

  // Main loop - 3DS style
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();

    switch (mode) {
    case APP_MODE_BOOK:
      UpdateStatus();
      HandleEventInBook();
      break;

    case APP_MODE_BROWSER:
      browser_handleevent();
      if (browser_view_dirty)
        browser_draw();
      break;

    case APP_MODE_QUIT:
      prefs->Write();
      return 0;
      break;

    case APP_MODE_PREFS:
      PrefsHandleEvent();
      if (prefs_view_dirty)
        PrefsDraw();
      break;

    case APP_MODE_PREFS_FONT:
    case APP_MODE_PREFS_FONT_BOLD:
    case APP_MODE_PREFS_FONT_ITALIC:
    case APP_MODE_PREFS_FONT_BOLDITALIC:
      fontmenu->handleInput();
      if (fontmenu->isDirty())
        fontmenu->draw();
      break;

    case APP_MODE_BOOKMARKS:
      bookmarkmenu->HandleInput(hidKeysDown());
      if (bookmarkmenu->IsDirty())
        bookmarkmenu->Draw();
      break;
    }

    // Copy software buffers to 3DS framebuffer
    ts->BlitToFramebuffer();

    gfxFlushBuffers();
    gfxSwapBuffers();
  }
  return 0;
}

void App::SetBrightness(u8 b) {
  // Not available on 3DS - brightness is system-controlled
  brightness = b % 4;
}

void App::CycleBrightness() {
  // Not available on 3DS
  ++brightness %= 4;
}

int App::FindBooks() {
  DIR *dp = opendir(bookdir.c_str());
  if (!dp) {
    // Try fallback paths
    const char *fallbacks[] = {"sdmc:/book", "sdmc:/books", NULL};
    for (int i = 0; fallbacks[i]; i++) {
      dp = opendir(fallbacks[i]);
      if (dp) {
        bookdir = std::string(fallbacks[i]);
        break;
      }
    }
    if (!dp)
      return 1;
  }

  struct dirent *ent;
  while ((ent = readdir(dp))) {
    char *filename = ent->d_name;
    if (*filename == '.')
      continue;
    // Starting from the end, find the file extension.
    char *c;
    for (c = filename + strlen(filename) - 1; c != filename && *c != '.'; c--)
      ;
    if (!strcmp(".epub", c)) {
      Book *book = new Book(this);
      book->SetFolderName(bookdir.c_str());
      book->SetFileName(filename);
      book->SetTitle(filename);
      book->format = FORMAT_EPUB;
      books.push_back(book);
      bookcount++;
    }
  }
  closedir(dp);
  return 0;
}

// 3DS touch input — map physical touch to our buffer coordinate system.
// BlitToFramebuffer maps buffer (sx,sy) → screen via:
//   physical_X = fbH - 1 - sy   (fbH=320 for bottom screen)
//   physical_Y = fbW - 1 - sx   (fbW=240 for bottom screen)
// So to reverse:  sx = 239 - touch.py,  sy = 319 - touch.px
// We store the result in px/py where px→sx (origin.x), py→sy (origin.y).
touchPosition App::TouchRead() {
  touchPosition raw;
  hidTouchRead(&raw);
  touchPosition mapped;
  mapped.px = 239 - raw.py; // → buffer sx = origin.x
  mapped.py = 319 - raw.px; // → buffer sy = origin.y
  return mapped;
}

void App::ShowFontView(int app_font_mode) {
  mode = app_font_mode;
  buttonprefs.Label("cancel");
  ts->SetScreen(ts->screenright);
  fontmenu->setDirty();
}

void App::ShowLibraryView() {
  mode = APP_MODE_BROWSER;
  buttonprefs.Label("settings");
  ts->SetScreen(ts->screenright);
  browser_view_dirty = true;
}

void App::ShowSettingsView(bool from_book) {
  prefs_book_context = from_book;
  u8 visible_count = PrefsVisibleButtonCount();
  if (visible_count == 0)
    visible_count = 1;
  if (prefsSelected >= visible_count)
    prefsSelected = visible_count - 1;
  mode = APP_MODE_PREFS;
  buttonprefs.Label(" library");
  ts->SetScreen(ts->screenright);
  prefs_view_dirty = true;
}

void App::ShowBookmarksView() {
  mode = APP_MODE_BOOKMARKS;
  ts->SetScreen(ts->screenright);
  bookmarkmenu->Init();
}

void App::UpdateStatus() {
  if (mode != APP_MODE_BOOK)
    return;
  u16 *screen = ts->GetScreen();
  time_t unixTime = time(NULL);
  struct tm *timeStruct = localtime(&unixTime);

  char tmsg[24];
  if (!timeStruct) {
    sprintf(tmsg, "--:--");
  } else if (prefs->time24h) {
    sprintf(tmsg, "%02d:%02d", timeStruct->tm_hour, timeStruct->tm_min);
  } else {
    int h = timeStruct->tm_hour % 12;
    if (h == 0)
      h = 12;
    sprintf(tmsg, "%02d:%02d %s", h, timeStruct->tm_min,
            timeStruct->tm_hour >= 12 ? "PM" : "AM");
  }

  // Draw on top screen (which is 240x400 in buffer)
  ts->SetScreen(ts->screenleft);
  int style = ts->GetStyle();
  int savedBottomMargin = ts->margin.bottom;
  // Status HUD is outside the text area and should ignore page margins.
  ts->margin.bottom = 0;
  ts->SetStyle(TEXT_STYLE_BROWSER); // smaller, readable font

  // Clear the status bar area at the bottom: y=380 to 400
  ts->ClearRect(0, 380, 240, 400);

  u16 fgColor = ts->GetFgColor();

  // Print Clock (Left)
  int textY = 384;
  ts->SetPen(8, textY);
  ts->PrintString(tmsg);
  int clockWidth = ts->GetStringWidth(tmsg, TEXT_STYLE_BROWSER);

  // Print Percentage and Page Number (Right)
  int pX = 232;
  if (bookcurrent && bookcurrent->GetPageCount() > 0) {
    int pageNum = bookcurrent->GetPosition();
    int pageCount = bookcurrent->GetPageCount();
    float percent = pageCount > 1
                        ? ((float)pageNum / (float)(pageCount - 1)) * 100.0f
                        : 100.0f;
    char pmsg[32];
    sprintf(pmsg, "%.1f%%", percent);
    int pw = ts->GetStringWidth(pmsg, TEXT_STYLE_BROWSER);
    pX = 232 - pw;
    ts->SetPen(pX, textY);
    ts->PrintString(pmsg);

    // Draw Progress Bar between clock and percentage
    int barStart = 8 + clockWidth + 12;
    int barEnd = pX - 12;
    if (barEnd > barStart + 10) {
      int barY = textY + 5;
      int barHeight = 8;
      // Draw border
      ts->DrawRect(barStart, barY, barEnd, barY + barHeight, fgColor);

      // Draw fill
      if (pageCount > 1 && pageNum > 0) {
        int fillW =
            (int)(((float)(barEnd - barStart - 4) * pageNum) / (pageCount - 1));
        if (fillW > 0) {
          ts->FillRect(barStart + 2, barY + 2, barStart + 2 + fillW,
                       barY + barHeight - 2, fgColor);
        }
      }
    }
  }

  ts->SetStyle(style);
  ts->margin.bottom = savedBottomMargin;
  ts->SetScreen(screen);
}

void App::SetOrientation(bool turned_right) {
  // On 3DS, orientation is fixed.
  // We keep the variable for preferences compatibility but don't
  // manipulate hardware registers.
  orientation = turned_right;

  if (turned_right) {
    key.down = KEY_UP;
    key.up = KEY_DOWN;
    key.left = KEY_RIGHT;
    key.right = KEY_LEFT;
    key.l = KEY_R;
    key.r = KEY_L;
  } else {
    key.down = KEY_DOWN;
    key.up = KEY_UP;
    key.left = KEY_LEFT;
    key.right = KEY_RIGHT;
    key.l = KEY_L;
    key.r = KEY_R;
  }
}

void App::InitScreens() {
  // consoleInit() set the bottom screen to single-buffered and may have
  // changed the pixel format.  Take full control back before the main loop.
  gfxSetDoubleBuffering(GFX_TOP, true);
  gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
  gfxSetDoubleBuffering(GFX_BOTTOM, true);
  gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);

  // Clear our software buffers.
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
}

void App::PrintStatus(const char *msg) {
  // Write status to log file since the console is disabled during rendering.
  FILE *f = fopen(LOGFILEPATH, "a");
  if (f) {
    time_t rawtime;
    struct tm *info;
    char buffer[80];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", info);

    fprintf(f, "[%s] %s\n", buffer, msg);
    fclose(f);
  }
}

void App::PrintStatus(std::string msg) { PrintStatus(msg.c_str()); }

void App::SetProgress(int amount) {
  // TODO: implement progress bar for 3DS
}

// parse_error is defined in app_book.cpp
