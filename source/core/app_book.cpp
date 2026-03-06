#include "app.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <expat.h>

#include <3ds.h>

#include "book.h"
#include "button.h"
#include "main.h"
#include "parse.h"
#include "text.h"

//! Book-related methods for App class.

void App::HandleEventInBook() {
  u16 pagecurrent = bookcurrent->GetPosition();
  u16 pagecount = bookcurrent->GetPageCount();
  bool status_dirty = false;

  // Use 3DS edge-triggered key state to avoid carry-over/repeat from the key
  // press used to open the book.
  u32 keys = hidKeysDown();

  if (keys & (KEY_A | key.r | key.down)) {
    // page forward.
    if (pagecurrent < pagecount - 1) {
      pagecurrent++;
      bookcurrent->SetPosition(pagecurrent);
      bookcurrent->GetPage()->Draw(ts);
      status_dirty = true;
    }
  } else if (keys & (KEY_B | key.l | key.up)) {
    // page back.
    if (pagecurrent > 0) {
      pagecurrent--;
      bookcurrent->SetPosition(pagecurrent);
      bookcurrent->GetPage()->Draw(ts);
      status_dirty = true;
    }
  } else if (keys & KEY_X) {
    // cycle color modes: 0=normal, 1=dark, 2=sepia
    int mode = ts->GetColorMode();
    ts->SetColorMode((mode + 1) % 3);
    bookcurrent->GetPage()->Draw(ts);
    status_dirty = true;
  } else if (keys & KEY_Y) {
    ToggleBookmark();
  } else if (keys & KEY_TOUCH) {
    // Turn page by touch zones on the physical touchscreen:
    //   upper half -> previous page
    //   lower half -> next page
    // In book orientation this matches: left/up = back, right/down = forward.
    touchPosition raw;
    hidTouchRead(&raw);
    const bool forward_zone = (raw.py >= 120); // bottom half (240px / 2)
    if (!forward_zone) {
      if (pagecurrent > 0) {
        pagecurrent--;
        bookcurrent->SetPosition(pagecurrent);
        bookcurrent->GetPage()->Draw(ts);
        status_dirty = true;
      }
    } else {
      if (pagecurrent < pagecount - 1) {
        pagecurrent++;
        bookcurrent->SetPosition(pagecurrent);
        bookcurrent->GetPage()->Draw(ts);
        status_dirty = true;
      }
    }
  } else if (keys & KEY_START) {
    // Return to browser without reparsing the current book later.
    // Keep one parsed book resident in memory for fast reopen.
    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->PrintSplash(ts->screenleft);
    ShowLibraryView();
    prefs->Write();
  } else if (keys & KEY_SELECT) {
    // Go directly to settings from book.
    ShowSettingsView(true);
    prefs->Write();
  } else if (keys & (key.right | key.left)) {
    // Navigate bookmarks.
    std::list<u16> *bookmarks = bookcurrent->GetBookmarks();

    if (!bookmarks->empty()) {
      // Find the bookmark just after the current page
      if (keys & key.left) {
        std::list<u16>::iterator i;
        for (i = bookmarks->begin(); i != bookmarks->end(); i++) {
          if (*i > bookcurrent->GetPosition())
            break;
        }

        if (i == bookmarks->end())
          i = bookmarks->begin();

        bookcurrent->SetPosition(*i);
      } else {
        std::list<u16>::reverse_iterator i;
        for (i = bookmarks->rbegin(); i != bookmarks->rend(); i++) {
          if (*i < bookcurrent->GetPosition())
            break;
        }

        if (i == bookmarks->rend())
          i = bookmarks->rbegin();

        bookcurrent->SetPosition(*i);
      }
      bookcurrent->GetPage()->Draw(ts);
      status_dirty = true;
    }
  }

  if (status_dirty)
    RequestStatusRedraw();
}

void App::ToggleBookmark() {
  // Toggle bookmark for the current page.
  std::list<u16> *bookmarks = bookcurrent->GetBookmarks();
  u16 pagecurrent = bookcurrent->GetPosition();

  bool found = false;
  for (std::list<u16>::iterator i = bookmarks->begin(); i != bookmarks->end();
       i++) {
    if (*i == pagecurrent) {
      bookmarks->erase(i);
      found = true;
      break;
    }
  }

  if (!found) {
    bookmarks->push_back(pagecurrent);
    bookmarks->sort();
  }

  bookcurrent->GetPage()->Draw(ts);
  RequestStatusRedraw();
}

void App::CloseBook() {
  if (!bookcurrent)
    return;
  bookcurrent->Close();
  bookcurrent = NULL;
}

int App::GetBookIndex(Book *b) {
  if (!b)
    return -1;
  std::vector<Book *>::iterator it;
  int i = 0;
  for (it = books.begin(); it < books.end(); it++, i++) {
    if (*it == b)
      return i;
  }
  return -1;
}

u8 App::OpenBook(void) {
  //! Attempt to open book indicated by bookselected.

  if (!bookselected)
    return 254;

  PrintStatus("opening book ...");
  if (bookselected->GetTitle())
    PrintStatus(bookselected->GetTitle());

  // Fast path: selected book is already parsed and resident.
  if (bookselected->GetPageCount() > 0) {
    bookcurrent = bookselected;
    if (mode == APP_MODE_BROWSER) {
      if (orientation) {
        // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
      }
      mode = APP_MODE_BOOK;
      PrintStatus("OpenBook: reused parsed book");
    }
    if (bookcurrent->GetPosition() >= bookcurrent->GetPageCount())
      bookcurrent->SetPosition(0);
    bookcurrent->GetPage()->Draw(ts);
    RequestStatusRedraw();
    prefs->Write();
    return 0;
  }

  auto drawOpeningSplash = [&]() {
    int savedStyle = ts->GetStyle();
    int savedColorMode = ts->GetColorMode();
    u16 *savedScreen = ts->GetScreen();

    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->SetColorMode(0);

    ts->SetScreen(ts->screenleft);
    ts->ClearScreen();
    ts->SetPen(12, 28);
    ts->PrintString("3dslibris");
    ts->SetPen(12, 52);
    ts->PrintString("opening book ...");

    ts->SetScreen(ts->screenright);
    ts->ClearScreen();
    ts->SetPen(12, 28);
    ts->PrintString("opening book ...");

    const char *name = bookselected->GetFileName();
    if (!name || !*name)
      name = bookselected->GetTitle();
    if (name && *name) {
      ts->SetPen(12, 50);
      ts->PrintString(name);
    }

    ts->SetStyle(savedStyle);
    ts->SetColorMode(savedColorMode);
    ts->SetScreen(savedScreen);

    if (ts->BlitToFramebuffer()) {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
  };

  // While parsing a new book, avoid displaying stale browser highlight state.
  drawOpeningSplash();

  if (bookcurrent && bookcurrent != bookselected)
    bookcurrent->Close();
  if (int err = bookselected->Open()) {
    char msg[64];
    sprintf(msg, "error (%d)", err);
    PrintStatus(msg);
    return err;
  }
  bookcurrent = bookselected;

  char msg[64];
  int pageCount = bookcurrent->GetPageCount();
  sprintf(msg, "Generated %d pages", pageCount);
  PrintStatus(msg);

  if (pageCount <= 0) {
    PrintStatus("error: book has no parsed pages");
    bookcurrent->Close();
    bookcurrent = nullptr;
    mode = APP_MODE_BROWSER;
    browser_view_dirty = true;
    return 253;
  }

  if (mode == APP_MODE_BROWSER) {
    if (orientation) {
      // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
    }
    mode = APP_MODE_BOOK;
    PrintStatus("OpenBook: switched mode to APP_MODE_BOOK");
  }

  if (bookcurrent->GetPosition() >= pageCount)
    bookcurrent->SetPosition(0);
  bookcurrent->GetPage()->Draw(ts);
  RequestStatusRedraw();
  prefs->Write();
  return 0;
}

void App::parse_error(XML_Parser p) {
  char msg[128];
  sprintf(msg, "%d:%d: %s\n", (int)XML_GetCurrentLineNumber(p),
          (int)XML_GetCurrentColumnNumber(p),
          XML_ErrorString(XML_GetErrorCode(p)));
  PrintStatus(msg);
}
