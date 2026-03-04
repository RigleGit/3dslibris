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

  auto keys = keysDown();

  if (keys & (KEY_A | key.r | key.down)) {
    // page forward.
    if (pagecurrent < pagecount - 1) {
      pagecurrent++;
      bookcurrent->SetPosition(pagecurrent);
      bookcurrent->GetPage()->Draw(ts);
    }
    prefs->Write();
  } else if (keys & (KEY_B | key.l | key.up)) {
    // page back.
    if (pagecurrent > 0) {
      pagecurrent--;
      bookcurrent->SetPosition(pagecurrent);
      bookcurrent->GetPage()->Draw(ts);
    }
    prefs->Write();
  } else if (keys & KEY_X) {
    // cycle color modes: 0=normal, 1=dark, 2=sepia
    int mode = ts->GetColorMode();
    ts->SetColorMode((mode + 1) % 3);
    bookcurrent->GetPage()->Draw(ts);
  } else if (keys & KEY_Y) {
    ToggleBookmark();
  } else if (keys & KEY_TOUCH) {
    // Turn page.
    touchPosition coord = TouchRead();
    if (coord.py < 95) {
      if (pagecurrent > 0) {
        pagecurrent--;
        bookcurrent->SetPosition(pagecurrent);
        bookcurrent->GetPage()->Draw(ts);
      }
    } else {
      if (pagecurrent < pagecount - 1) {
        pagecurrent++;
        bookcurrent->SetPosition(pagecurrent);
        bookcurrent->GetPage()->Draw(ts);
      }
    }
    prefs->Write();
  } else if (keys & KEY_START) {
    bookcurrent->Close();
    bookcurrent = nullptr;

    // return to browser.
    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->PrintSplash(ts->screenleft);
    ShowLibraryView();
    prefs->Write();
  } else if (keys & KEY_SELECT) {
    // Go directly to settings from book.
    ShowSettingsView();
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
    }
  }
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
  printf("Opening book: %s\n", bookselected->GetTitle());
  gfxFlushBuffers();
  gfxSwapBuffers();

  if (bookcurrent)
    bookcurrent->Close();
  if (int err = bookselected->Open()) {
    char msg[64];
    sprintf(msg, "error (%d)", err);
    PrintStatus(msg);
    return err;
  }
  bookcurrent = bookselected;
  if (mode == APP_MODE_BROWSER) {
    if (orientation)
      // lcdSwap();
      mode = APP_MODE_BOOK;
  }

  char msg[64];
  sprintf(msg, "Generated %d pages", bookcurrent->GetPageCount());
  PrintStatus(msg);

  if (bookcurrent->GetPosition() >= bookcurrent->GetPageCount())
    bookcurrent->SetPosition(0);
  bookcurrent->GetPage()->Draw(ts);
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
