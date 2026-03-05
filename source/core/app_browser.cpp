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

namespace {

static std::string TrimSpaces(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && s[start] == ' ')
    start++;
  size_t end = s.size();
  while (end > start && s[end - 1] == ' ')
    end--;
  return s.substr(start, end - start);
}

static bool LooksLikeValidUtf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    if (i + need >= s.size())
      return false;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80)
        return false;
    }
    i += need + 1;
  }
  return true;
}

static std::string Latin1ToUtf8(const std::string &in) {
  std::string out;
  out.reserve(in.size() * 2);
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      out.push_back((char)c);
    } else {
      out.push_back((char)(0xC0 | (c >> 6)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

static bool TryRepairMojibakeUtf8(const std::string &in, std::string *out) {
  if (!out)
    return false;

  // Repair common "UTF-8 bytes decoded as Latin-1 then re-encoded as UTF-8"
  // mojibake patterns.
  std::string collapsed;
  collapsed.reserve(in.size());

  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      collapsed.push_back((char)c);
      i++;
      continue;
    }

    if (i + 1 >= in.size())
      return false;

    unsigned char c2 = (unsigned char)in[i + 1];
    if (c == 0xC2 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)c2);
      i += 2;
      continue;
    }
    if (c == 0xC3 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)(c2 + 0x40));
      i += 2;
      continue;
    }
    return false;
  }

  if (!LooksLikeValidUtf8(collapsed))
    return false;
  *out = collapsed;
  return true;
}

static std::string BuildFallbackTitle(Book *book) {
  if (!book)
    return "";
  const char *raw = book->GetFileName();
  std::string name = raw ? raw : "";
  if (!LooksLikeValidUtf8(name)) {
    name = Latin1ToUtf8(name);
  } else {
    std::string repaired;
    if (TryRepairMojibakeUtf8(name, &repaired))
      name = repaired;
  }

  // Prefer filename for missing covers, strip extension.
  size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name = name.substr(0, dot);

  for (size_t i = 0; i < name.size(); i++) {
    if (name[i] == '_' || name[i] == '-')
      name[i] = ' ';
  }

  std::string compact;
  compact.reserve(name.size());
  bool prev_space = true;
  for (size_t i = 0; i < name.size(); i++) {
    bool is_space = (name[i] == ' ');
    if (is_space && prev_space)
      continue;
    compact.push_back(name[i]);
    prev_space = is_space;
  }
  return TrimSpaces(compact);
}

static size_t Utf8BytesForCharCount(const char *s, size_t char_count) {
  if (!s)
    return 0;
  size_t bytes = 0;
  size_t chars = 0;
  while (s[bytes] && chars < char_count) {
    unsigned char c = (unsigned char)s[bytes];
    size_t step = 1;
    if ((c & 0x80) == 0x00)
      step = 1;
    else if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;

    // Clamp malformed/truncated sequences to avoid overrun.
    for (size_t i = 1; i < step; i++) {
      if (!s[bytes + i]) {
        step = i;
        break;
      }
    }
    bytes += step;
    chars++;
  }
  return bytes;
}

static void DrawWrappedTitleInsideCover(Text *ts, const std::string &title,
                                        int x, int y, int w, int h, u8 style) {
  if (!ts || title.empty() || w <= 8 || h <= 8)
    return;

  int line_h = ts->GetHeight();
  int max_lines = (h - 8) / MAX(1, line_h);
  if (max_lines < 1)
    return;

  size_t pos = 0;
  int drawn = 0;
  while (pos < title.size() && drawn < max_lines) {
    while (pos < title.size() && title[pos] == ' ')
      pos++;
    if (pos >= title.size())
      break;

    u8 fit = ts->GetCharCountInsideWidth(title.c_str() + pos, style, w - 8);
    if (!fit)
      break;

    size_t take = Utf8BytesForCharCount(title.c_str() + pos, fit);
    if (pos + take < title.size()) {
      size_t back = take;
      while (back > 0 && title[pos + back - 1] != ' ')
        back--;
      if (back > 0)
        take = back;
    }
    std::string line = TrimSpaces(title.substr(pos, take));
    if (line.empty()) {
      pos += take;
      continue;
    }

    ts->SetPen(x + 4, y + 4 + drawn * line_h);
    ts->PrintString(line.c_str(), style);
    drawn++;
    pos += take;
  }
}

} // namespace

void App::browser_handleevent() {
  u32 keys = hidKeysDown();
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y |
                           KEY_START | KEY_SELECT | KEY_UP | KEY_DOWN |
                           KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R |
                           KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT |
                           KEY_CPAD_RIGHT;
  if (browser_wait_input_release) {
    if (hidKeysHeld() & release_mask)
      return;
    browser_wait_input_release = false;
    return;
  }

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
    const int kGridCols = 2;
    const int kGridRows = 2;
    const int kCellW = 115;
    const int kCellH = 140;
    const int kGridX0 = 5;
    const int kGridY0 = 3;

    auto hitsButtonAt = [&](Button &button, int px, int py, int slack) {
      if (slack <= 0)
        return button.EnclosesPoint((u16)px, (u16)py);
      for (int dy = -slack; dy <= slack; dy += slack) {
        for (int dx = -slack; dx <= slack; dx += slack) {
          int x = px + dx;
          int y = py + dy;
          if (x < 0 || y < 0 || x > 239 || y > 319)
            continue;
          if (button.EnclosesPoint((u16)x, (u16)y))
            return true;
        }
      }
      return false;
    };

    auto handleTouchAt = [&](int x, int y) -> bool {
      if (x < 0 || y < 0 || x > 239 || y > 319)
        return false;

      if (hitsButtonAt(buttonnext, x, y, 4)) {
        browser_nextpage();
        return true;
      }
      if (hitsButtonAt(buttonprev, x, y, 4)) {
        browser_prevpage();
        return true;
      }
      if (hitsButtonAt(buttonprefs, x, y, 4)) {
        ShowSettingsView(false);
        return true;
      }

      // Prefer coarse cell hit-test (cover + title/progress area):
      // single tap selects, tapping selected book opens.
      if (x >= kGridX0 && y >= kGridY0) {
        int col = (x - kGridX0) / kCellW;
        int row = (y - kGridY0) / kCellH;
        if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows) {
          int page_idx = row * kGridCols + col;
          if (page_idx >= 0 && page_idx < APP_BROWSER_BUTTON_COUNT) {
            int book_idx = browserstart + page_idx;
            if (book_idx >= 0 && book_idx < bookcount) {
              if (bookselected == books[book_idx]) {
                OpenBook();
              } else {
                bookselected = books[book_idx];
                browser_view_dirty = true;
              }
              return true;
            }
          }
        }
      }

      // Fallback to original cover hitboxes.
      for (int i = browserstart;
           (i < bookcount) && (i < browserstart + APP_BROWSER_BUTTON_COUNT);
           i++) {
        if (hitsButtonAt(*buttons[i], x, y, 4)) {
          if (bookselected == books[i]) {
            OpenBook();
          } else {
            bookselected = books[i];
            browser_view_dirty = true;
          }
          return true;
        }
      }
      return false;
    };

    // Browser touch must use a single mapping. Trying alternate mappings here
    // causes false positives (touches interpreted as other UI zones).
    touchPosition mapped = TouchRead();
    handleTouchAt((int)mapped.px, (int)mapped.py);
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
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % GRID_COLS;
    int row = page_idx / GRID_COLS;

    buttons.push_back(new Button());
    buttons[i]->Init(ts);
    // Button is the cover area - portrait orientation
    buttons[i]->Resize(COVER_W + 4, COVER_H + 4);
    buttons[i]->Move(GRID_X0 + col * CELL_W, GRID_Y0 + row * CELL_H);

    // Cover extraction moved to browser_draw to avoid freezing at startup

    // In browser_draw we render fallback title manually for no-cover books.
    buttons[i]->SetLabel1(std::string(""));
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
  if (browserstart >= APP_BROWSER_BUTTON_COUNT) {
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

    // Draw title:
    //  - with cover: below thumbnail (single line)
    //  - without cover: wrapped inside thumbnail rectangle
    ts->SetPixelSize(10);
    if (books[i]->coverPixels) {
      const char *title = books[i]->GetTitle();
      if (title && strlen(title)) {
        char truncTitle[20];
        strncpy(truncTitle, title, 19);
        truncTitle[19] = '\0';
        ts->SetPen(btnX, btnY + COVER_H + 12);
        ts->PrintString(truncTitle);
      }
    } else {
      std::string fallback_title = BuildFallbackTitle(books[i]);
      DrawWrappedTitleInsideCover(ts, fallback_title, btnX + 2, btnY + 2,
                                  COVER_W, COVER_H, TEXT_STYLE_BROWSER);
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
