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
#include <3ds/util/utf.h>

#include "book.h"
#include "bookmark_menu.h"
#include "chapter_menu.h"
#include "button.h"
#include "font.h"
#include "main.h"
#include "parse.h"
#include "text.h"
#include "version.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

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
  browser_wait_input_release = false;

  prefs = new Prefs(this);
  prefsSelected = -1;
  prefs_view_dirty = false;
  prefs_book_context = false;
  status_last_minute = -1;
  status_last_percent_tenths = -1;
  status_force_redraw = true;

  ts = new Text();
  ts->app = this;

  fontmenu = new FontMenu(this);
  bookmarkmenu = new BookmarkMenu(this);
  chaptermenu = new ChapterMenu(this);
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
  delete chaptermenu;
}

// std::sort comparator: books by title
static bool book_title_lessthan(Book *a, Book *b) {
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

#if UTF8_FILENAME_DIAG
static bool looks_like_valid_utf8(const char *s) {
  if (!s)
    return false;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned char c = *p;
    if ((c & 0x80) == 0x00) {
      p++;
      continue;
    }
    int need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    p++;
    for (int i = 0; i < need; i++, p++) {
      if (!*p || ((*p & 0xC0) != 0x80))
        return false;
    }
  }
  return true;
}

static std::string hex_bytes_for_log(const char *s, size_t max_bytes = 32) {
  static const char hex[] = "0123456789ABCDEF";
  if (!s)
    return "";
  std::string out;
  size_t n = strlen(s);
  if (n > max_bytes)
    n = max_bytes;
  out.reserve(n * 3 + 8);
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)s[i];
    if (i)
      out.push_back(' ');
    out.push_back(hex[(b >> 4) & 0x0F]);
    out.push_back(hex[b & 0x0F]);
  }
  if (strlen(s) > max_bytes)
    out += " ...";
  return out;
}
#endif

static void log_filename_stage(App *app, const char *stage, const char *value) {
#if !UTF8_FILENAME_DIAG
  (void)app;
  (void)stage;
  (void)value;
  return;
#else
  if (!app || !stage || !value)
    return;
  char msg[512];
  std::string bytes = hex_bytes_for_log(value);
  snprintf(msg, sizeof(msg),
           "FindBooks %-20s len=%u valid=%d bytes=[%s] text=\"%s\"", stage,
           (unsigned)strlen(value), looks_like_valid_utf8(value) ? 1 : 0,
           bytes.c_str(), value);
  app->PrintStatus(msg);
#endif
}

static format_t detect_book_format(const char *filename) {
  if (!filename)
    return FORMAT_UNDEF;
  size_t len = strlen(filename);
  if (len >= 5 && strcasecmp(filename + len - 5, ".epub") == 0)
    return FORMAT_EPUB;
  if (len >= 4 && strcasecmp(filename + len - 4, ".fb2") == 0)
    return FORMAT_XHTML;
  if (len >= 4 && strcasecmp(filename + len - 4, ".txt") == 0)
    return FORMAT_XHTML;
  if (len >= 4 && strcasecmp(filename + len - 4, ".rtf") == 0)
    return FORMAT_XHTML;
  return FORMAT_UNDEF;
}

static std::string sdmc_to_archive_relpath(const std::string &path) {
  const char kPrefix[] = "sdmc:";
  if (path.compare(0, strlen(kPrefix), kPrefix) == 0) {
    std::string rel = path.substr(strlen(kPrefix));
    if (rel.empty())
      return "/";
    if (rel[0] != '/')
      rel.insert(rel.begin(), '/');
    return rel;
  }
  if (!path.empty() && path[0] != '/')
    return std::string("/") + path;
  return path;
}

static bool looks_like_valid_utf8_bytes(const std::string &s) {
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

static std::string normalize_fs_filename_for_io(const char *raw_name) {
  if (!raw_name)
    return "";
  std::string in(raw_name);
  std::string out;
  out.reserve(in.size());
  bool changed = false;
  std::vector<unsigned char> mapped;
  mapped.reserve(in.size() / 3);

  for (size_t i = 0; i < in.size();) {
    unsigned char b0 = (unsigned char)in[i];
    if (i + 2 < in.size() && b0 == 0xEF) {
      unsigned char b1 = (unsigned char)in[i + 1];
      unsigned char b2 = (unsigned char)in[i + 2];
      if (b1 >= 0xBC && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
        unsigned char recovered =
            (unsigned char)(((b1 - 0xBC) << 6) | (b2 - 0x80));
        out.push_back((char)recovered);
        mapped.push_back(recovered);
        i += 3;
        changed = true;
        continue;
      }
    }
    out.push_back((char)b0);
    i++;
  }

  if (!changed || mapped.size() < 2)
    return in;

  bool has_utf8_pair = false;
  for (size_t i = 0; i + 1 < mapped.size(); i++) {
    unsigned char lead = mapped[i];
    unsigned char cont = mapped[i + 1];
    if (lead >= 0xC2 && lead <= 0xF4 && cont >= 0x80 && cont <= 0xBF) {
      has_utf8_pair = true;
      break;
    }
  }
  if (!has_utf8_pair)
    return in;
  if (!looks_like_valid_utf8_bytes(out))
    return in;
  return out;
}

static bool utf16_name_to_utf8(const u16 *name, std::string *out) {
  if (!name || !out)
    return false;

  size_t in_len = 0;
  while (in_len < 0x106 && name[in_len] != 0)
    in_len++;

  if (in_len == 0) {
    out->clear();
    return true;
  }

  std::vector<uint8_t> utf8buf(in_len * 4 + 4, 0);
  ssize_t produced = utf16_to_utf8(utf8buf.data(), name, utf8buf.size() - 1);
  if (produced < 0)
    return false;

  size_t out_len = (size_t)produced;
  if (out_len > utf8buf.size() - 1)
    out_len = utf8buf.size() - 1;
  out->assign((const char *)utf8buf.data(), out_len);
  return true;
}

static void append_book_from_filename(App *app, const char *filename) {
  if (!app || !filename || !*filename)
    return;
  if (filename[0] == '.')
    return;
  format_t format = detect_book_format(filename);
  if (format == FORMAT_UNDEF)
    return;

  std::string raw_name(filename);
  std::string io_name = normalize_fs_filename_for_io(filename);
  log_filename_stage(app, "d_name", raw_name.c_str());
  if (io_name != raw_name) {
    log_filename_stage(app, "d_name_io_fix", io_name.c_str());
  }
  Book *book = new Book(app);
  book->SetFolderName(app->bookdir.c_str());
  book->SetFileName(io_name.c_str());
  book->SetTitle(io_name.c_str());
  log_filename_stage(app, "book.filename", book->GetFileName());
  log_filename_stage(app, "book.title", book->GetTitle());
  book->format = format;
  app->books.push_back(book);
  app->bookcount++;
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

    if (ts->BlitToFramebuffer()) {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
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
  u64 t_scan_ms = osGetTime();
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
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "TIMING: scan_books=%llums count=%u",
             (unsigned long long)(osGetTime() - t_scan_ms),
             (unsigned)bookcount);
    PrintStatus(msg);
  }

  std::sort(books.begin(), books.end(), &book_title_lessthan);

  prefs->Read();
  drawBootStatus("Preparing library...", "");
  // Apply key mapping/orientation loaded from prefs.
  SetOrientation(orientation);
  PrintStatus("Preparing library...");
  u64 t_prepare_ms = osGetTime();
  for (auto &book : books) {
    book->GetBookmarks()->sort();
  }
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "TIMING: prepare_library=%llums",
             (unsigned long long)(osGetTime() - t_prepare_ms));
    PrintStatus(msg);
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
    const char *title = bookcurrent->GetTitle();
    drawBootStatus("Opening last book...", (title && *title) ? title : "");
    OpenBook();
  }

  // Main loop - 3DS style
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    ProcessJobs(3); // Cooperative budget per frame (ms).

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

    case APP_MODE_CHAPTERS:
      chaptermenu->HandleInput(hidKeysDown());
      if (chaptermenu->IsDirty())
        chaptermenu->Draw();
      break;
    }

    // Copy software buffers to 3DS framebuffer only when something changed.
    if (ts->BlitToFramebuffer()) {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
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
  auto scan_with_native_fs = [&](const std::string &dir) -> int {
    FS_Archive sdmc_archive;
    Result rc = FSUSER_OpenArchive(&sdmc_archive, ARCHIVE_SDMC,
                                   fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(rc))
      return 1;

    std::string rel_path = sdmc_to_archive_relpath(dir);
    Handle dir_handle = 0;
    rc = FSUSER_OpenDirectory(&dir_handle, sdmc_archive,
                              fsMakePath(PATH_ASCII, rel_path.c_str()));
    if (R_FAILED(rc)) {
      FSUSER_CloseArchive(sdmc_archive);
      return 1;
    }

    while (true) {
      FS_DirectoryEntry entries[16];
      u32 read_count = 0;
      rc = FSDIR_Read(dir_handle, &read_count, 16, entries);
      if (R_FAILED(rc) || read_count == 0)
        break;

      for (u32 i = 0; i < read_count; i++) {
        if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)
          continue;

        std::string filename;
        if (!utf16_name_to_utf8(entries[i].name, &filename))
          continue;
        if (filename.empty())
          continue;
        append_book_from_filename(this, filename.c_str());
      }
    }

    FSDIR_Close(dir_handle);
    FSUSER_CloseArchive(sdmc_archive);
    return 0;
  };

  auto scan_with_posix_fallback = [&](const std::string &dir) -> int {
    DIR *dp = opendir(dir.c_str());
    if (!dp)
      return 1;

    struct dirent *ent;
    while ((ent = readdir(dp))) {
      append_book_from_filename(this, ent->d_name);
    }
    closedir(dp);
    return 0;
  };

  // Prefer native FS API (UTF-16 filenames), keep POSIX as safety fallback.
  if (scan_with_native_fs(bookdir) == 0)
    return 0;

  const char *fallbacks[] = {"sdmc:/book", "sdmc:/books", NULL};
  for (int i = 0; fallbacks[i]; i++) {
    if (scan_with_native_fs(fallbacks[i]) == 0) {
      bookdir = std::string(fallbacks[i]);
      return 0;
    }
  }

  if (scan_with_posix_fallback(bookdir) == 0)
    return 0;

  for (int i = 0; fallbacks[i]; i++) {
    if (scan_with_posix_fallback(fallbacks[i]) == 0) {
      bookdir = std::string(fallbacks[i]);
      return 0;
    }
  }

  return 1;
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
  browser_wait_input_release = true;
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

void App::ShowChaptersView() {
  if (bookcurrent && bookcurrent->format == FORMAT_EPUB &&
      !bookcurrent->tocResolveTried) {
    QueueTocResolve(bookcurrent);
  }
  mode = APP_MODE_CHAPTERS;
  ts->SetScreen(ts->screenright);
  chaptermenu->Init();
}

void App::RequestStatusRedraw() { status_force_redraw = true; }

void App::UpdateStatus() {
  if (mode != APP_MODE_BOOK)
    return;
  u16 *screen = ts->GetScreen();
  time_t unixTime = time(NULL);
  struct tm *timeStruct = localtime(&unixTime);
  int minute_of_day = -1;
  if (timeStruct) {
    minute_of_day = timeStruct->tm_hour * 60 + timeStruct->tm_min;
  }

  int percent_tenths = -1;
  if (bookcurrent && bookcurrent->GetPageCount() > 0) {
    int pageNum = bookcurrent->GetPosition();
    int pageCount = bookcurrent->GetPageCount();
    float percent = pageCount > 1
                        ? ((float)pageNum / (float)(pageCount - 1)) * 100.0f
                        : 100.0f;
    percent_tenths = (int)(percent * 10.0f + 0.5f);
  }

  if (!status_force_redraw && minute_of_day == status_last_minute &&
      percent_tenths == status_last_percent_tenths) {
    return;
  }

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
  status_last_minute = minute_of_day;
  status_last_percent_tenths = percent_tenths;
  status_force_redraw = false;
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
