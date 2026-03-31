/*
    3dslibris - app.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Replaced NDS hardware paths with 3DS/libctru equivalents.
    - Added startup flow, cover cache prep, and runtime timing telemetry.
    - Added 3DS status redraw control and bottom-screen gradient helpers.
*/

#include "app/app.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>

#include "book/book.h"
#include "menus/bookmark_menu.h"
#include "ui/button.h"
#include "menus/chapter_menu.h"
#include "shared/app_flow_utils.h"
#include "settings/font.h"
#include "debug_log.h"
#include "main.h"
#include "parse.h"
#include "settings/prefs.h"
#include "ui/text.h"
#include "shared/utf8_utils.h"
#include "version.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

#ifndef ORIENTATION_DIAG
#define ORIENTATION_DIAG 0
#endif

namespace {} // end anonymous namespace
#include "color_utils.h"
namespace {

struct RuntimeFileCheck {
  const char *path;
  bool directory;
  const char *label;
};

static format_t ToBookFormat(app_flow_utils::BookFileFormat format) {
  switch (format) {
  case app_flow_utils::BookFileFormat::Epub:
    return FORMAT_EPUB;
  case app_flow_utils::BookFileFormat::MuPdf:
    return FORMAT_PDF;
  case app_flow_utils::BookFileFormat::Cbz:
    return FORMAT_CBZ;
  case app_flow_utils::BookFileFormat::XhtmlLike:
    return FORMAT_XHTML;
  case app_flow_utils::BookFileFormat::Unsupported:
  default:
    return FORMAT_UNDEF;
  }
}

static bool PathExistsAndType(const char *path, bool want_dir) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (want_dir)
    return S_ISDIR(st.st_mode);
  return S_ISREG(st.st_mode);
}

static bool RuntimePathExistsEither(const char *sdmc_path,
                                    const char *romfs_path, bool want_dir) {
  return PathExistsAndType(sdmc_path, want_dir) ||
         PathExistsAndType(romfs_path, want_dir);
}

static std::string ResolveDefaultFontDir() {
  static const char *kSdmcFontDir = "sdmc:/3ds/3dslibris/font";
  static const char *kRomfsFontDir = "romfs:/3ds/3dslibris/font";
  static const char *kProbeFont = "LiberationSerif-Regular.ttf";

  std::string sdmc_probe = std::string(kSdmcFontDir) + "/" + kProbeFont;
  if (PathExistsAndType(sdmc_probe.c_str(), false))
    return std::string(kSdmcFontDir);

  std::string romfs_probe = std::string(kRomfsFontDir) + "/" + kProbeFont;
  if (PathExistsAndType(romfs_probe.c_str(), false))
    return std::string(kRomfsFontDir);

  return std::string(kSdmcFontDir);
}

static void NormalizeRuntimeAssetPaths(App *app) {
  if (!app)
    return;
  if (!PathExistsAndType(app->fontdir.c_str(), true))
    app->fontdir = ResolveDefaultFontDir();
}

static void CollectMissingRuntimeFiles(std::vector<std::string> *missing) {
  if (!missing)
    return;
  missing->clear();

  static const RuntimeFileCheck kRequired[] = {
      {"sdmc:/3ds/3dslibris/book", true, "book/"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Regular.ttf", false,
       "font/LiberationSerif-Regular.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Bold.ttf", false,
       "font/LiberationSerif-Bold.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Italic.ttf", false,
       "font/LiberationSerif-Italic.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-BoldItalic.ttf", false,
       "font/LiberationSerif-BoldItalic.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSans-Regular.ttf", false,
       "font/LiberationSans-Regular.ttf"},
  };

  if (!PathExistsAndType(kRequired[0].path, kRequired[0].directory))
    missing->push_back(kRequired[0].label);

  struct RuntimeFallbackFile {
    const char *sdmc_path;
    const char *romfs_path;
    const char *label;
  };
  static const RuntimeFallbackFile kBundled[] = {
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Regular.ttf",
       "romfs:/3ds/3dslibris/font/LiberationSerif-Regular.ttf",
       "font/LiberationSerif-Regular.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Bold.ttf",
       "romfs:/3ds/3dslibris/font/LiberationSerif-Bold.ttf",
       "font/LiberationSerif-Bold.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-Italic.ttf",
       "romfs:/3ds/3dslibris/font/LiberationSerif-Italic.ttf",
       "font/LiberationSerif-Italic.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSerif-BoldItalic.ttf",
       "romfs:/3ds/3dslibris/font/LiberationSerif-BoldItalic.ttf",
       "font/LiberationSerif-BoldItalic.ttf"},
      {"sdmc:/3ds/3dslibris/font/LiberationSans-Regular.ttf",
       "romfs:/3ds/3dslibris/font/LiberationSans-Regular.ttf",
       "font/LiberationSans-Regular.ttf"},
  };

  for (size_t i = 0; i < sizeof(kBundled) / sizeof(kBundled[0]); i++) {
    if (!RuntimePathExistsEither(kBundled[i].sdmc_path, kBundled[i].romfs_path,
                                 false))
      missing->push_back(kBundled[i].label);
  }
}

static void PrintInstallHelpToConsole(const std::vector<std::string> &missing) {
  printf("\n[FAIL] Incomplete SD install for 3dslibris.\n\n");
  printf("Download and extract:\n");
  printf("  3dslibris-sdmc.zip\n");
  printf("from GitHub Releases into the SD root:\n");
  printf("  sdmc:/\n\n");
  printf("Expected layout:\n");
  printf("  sdmc:/3ds/3dslibris/3dslibris.3dsx\n");
  printf("  sdmc:/3ds/3dslibris/book/\n");
  printf("  sdmc:/3ds/3dslibris/font/\n");
  printf("  sdmc:/3ds/3dslibris/resources/\n\n");
  if (!missing.empty()) {
    printf("Missing files:\n");
    size_t shown = std::min<size_t>(missing.size(), 8);
    for (size_t i = 0; i < shown; i++)
      printf("  %s\n", missing[i].c_str());
    if (missing.size() > shown)
      printf("  ... and %u more\n", (unsigned)(missing.size() - shown));
    printf("\n");
  }
}

#if ORIENTATION_DIAG
static int g_orientation_touch_diag_budget = 0;
#endif

} // namespace

App::App() {
  melonds = false;

  fontdir = ResolveDefaultFontDir();
  bookdir = std::string("sdmc:/3ds/3dslibris/book");
  bookcurrent_ = NULL;
  reopen = true;
  mode_ = AppMode::Browser;
  cache = false;
  orientation = false;
  paraspacing = 1;
  paraindent = 0;
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

  browser_.selected_book = NULL;
  browser_.page_start = 0;
  browser_.view_dirty = false;
  browser_.wait_input_release = false;

  prefs = new Prefs(this);
  prefs_view_.selected_index = -1;
  prefs_view_.view_dirty = false;
  prefs_view_.from_book = false;
  prefs_view_.layout_notice_pending = false;
  status_.last_minute = -1;
  status_.last_percent_tenths = -1;
  status_.progress_lock_book = NULL;
  status_.progress_pagecount_lock = 0;
  status_.force_redraw = true;
  opening_ = OpeningState();
  layout_revision = 0;
  pdf_touch_drag_active_ = false;
  pdf_touch_last_x_ = -1;
  pdf_touch_last_y_ = -1;
  pdf_deferred_ready_at_ms_ = 0;
  mobi_deferred_ready_at_ms_ = 0;

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
  UiButtonSkin_Exit();
}

bool App::IsFontMode(AppMode mode) {
  return mode == AppMode::PrefsFont || mode == AppMode::PrefsFontBold ||
         mode == AppMode::PrefsFontItalic ||
         mode == AppMode::PrefsFontBoldItalic;
}

AppMode App::GetMode() const { return mode_; }

void App::SetMode(AppMode mode) { mode_ = mode; }

Book *App::GetSelectedBook() const { return browser_.selected_book; }

void App::SetSelectedBook(Book *book) { browser_.selected_book = book; }

Book *App::GetCurrentBook() const { return bookcurrent_; }

void App::SetCurrentBook(Book *book) { bookcurrent_ = book; }

int App::BookCount() const { return (int)books.size(); }

int App::GetSelectedBookIndex() const {
  if (!browser_.selected_book)
    return -1;
  for (size_t i = 0; i < books.size(); i++) {
    if (books[i] == browser_.selected_book)
      return (int)i;
  }
  return -1;
}

int App::GetBrowserPageStart() const { return browser_.page_start; }

void App::SetBrowserPageStart(int page_start) {
  if (page_start < 0)
    page_start = 0;
  browser_.page_start = page_start;
}

void App::MarkBrowserDirty() { browser_.view_dirty = true; }

void App::MarkPrefsDirty() { prefs_view_.view_dirty = true; }

bool App::IsPrefsDirty() const { return prefs_view_.view_dirty; }

bool App::IsBrowserDirty() const { return browser_.view_dirty; }

// std::sort comparator: books by title
static bool book_title_lessthan(Book *a, Book *b) {
  return strcasecmp(a->GetTitle(), b->GetTitle()) < 0;
}

#if UTF8_FILENAME_DIAG
static bool looks_like_valid_utf8(const char *s) {
  return utf8_utils::IsValidUtf8(s);
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
  DBG_LOG(app, msg);
#endif
}

static std::string sdmc_to_archive_relpath(const std::string &path) {
  return app_flow_utils::SdmcToArchiveRelPath(path);
}

static std::string normalize_fs_filename_for_io(const char *raw_name) {
  if (!raw_name)
    return "";
  return utf8_utils::NormalizeFsFilenameForIo(raw_name);
}

static bool utf16_name_to_utf8(const u16 *name, std::string *out) {
  return utf8_utils::Utf16NameToUtf8(reinterpret_cast<const uint16_t *>(name),
                                     out);
}

static void append_book_from_filename(App *app, const char *filename) {
  if (!app || !app_flow_utils::ShouldIndexBookFilename(filename))
    return;
  format_t format =
      ToBookFormat(app_flow_utils::DetectBookFormat(filename));
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
}

int App::Run(void) {
  const int ok = 0;
  auto drawBootStatus = [&](const char *title, const std::vector<std::string> &lines,
                            bool fatal) {
    int savedStyle = ts->GetStyle();
    int savedColorMode = ts->GetColorMode();
    u16 *savedScreen = ts->GetScreen();
    int savedPixelSize = ts->pixelsize;

    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->SetColorMode(0);
    ts->SetPixelSize(10);

    ts->SetScreen(ts->screenleft);
    ts->PrintSplash(ts->screenleft);

    ts->SetScreen(ts->screenright);
    ts->ClearScreen();
    DrawBottomGradientBackground();
    ts->DrawRect(8, 10, 232, fatal ? 76 : 64, 0xC618);
    ts->SetPixelSize(14);
    ts->SetPen(14, 20);
    ts->PrintString(title && *title ? title : "Booting");
    ts->SetPixelSize(10);
    for (size_t i = 0; i < lines.size(); i++) {
      ts->SetPen(14, 84 + (int)i * 18);
      ts->PrintString(lines[i].c_str());
    }

    // Simple progress rail (visual feedback while booting).
    if (!fatal)
      ts->DrawRect(14, 138, 226, 152, 0xBDF7);
    else {
      ts->SetPen(14, 216);
      ts->PrintString("Pulsa START para salir");
    }

    ts->SetStyle(savedStyle);
    ts->SetColorMode(savedColorMode);
    ts->SetScreen(savedScreen);
    ts->SetPixelSize(savedPixelSize);

    if (ts->BlitToFramebuffer()) {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
  };

  auto haltOnFatalBootStatus = [&]() -> int {
    halt(-1);
    return 0;
  };

  // Start up typesetter.
  printf("Loading fonts...\n");
  if (ts->Init() != ok) {
    std::vector<std::string> missing;
    CollectMissingRuntimeFiles(&missing);
    PrintInstallHelpToConsole(missing);
    return 1;
  }

  // Initialize screens for 3DS.
  InitScreens();
  ts->SetStyle(TEXT_STYLE_BROWSER);
  drawBootStatus("Booting", {"Searching for books..."}, false);

  std::vector<std::string> missing_runtime;
  CollectMissingRuntimeFiles(&missing_runtime);
  if (!missing_runtime.empty()) {
    PrintStatus("error: incomplete sdmc install");
    std::vector<std::string> lines;
    lines.push_back("Download 3dslibris-sdmc.zip");
    lines.push_back("and extract it to sdmc:/");
    lines.push_back("Expected path: sdmc:/3ds/3dslibris/");
    lines.push_back("Missing files from the SD package");
    lines.push_back(missing_runtime[0]);
    if (missing_runtime.size() > 1) {
      char extra[48];
      snprintf(extra, sizeof(extra), "+%u more",
               (unsigned)(missing_runtime.size() - 1));
      lines.push_back(extra);
    }
    drawBootStatus("Incomplete installation", lines, true);
    return haltOnFatalBootStatus();
  }

  // Construct library.
  DBG_LOG(this, "Searching for books...");
#ifdef DSLIBRIS_DEBUG
  u64 t_scan_ms = osGetTime();
#endif
  if (FindBooks() != ok) {
    PrintStatus("error: no book directory");
    drawBootStatus("Incomplete installation",
                   {"Download 3dslibris-sdmc.zip",
                    "and extract it to sdmc:/",
                    "Expected folder: sdmc:/3ds/3dslibris/book"},
                   true);
    return haltOnFatalBootStatus();
  }
  if (BookCount() == 0) {
    PrintStatus("error: no epub files found");
    drawBootStatus("No books found",
                   {"Put your EPUB/FB2/TXT/RTF/ODT files",
                    "in sdmc:/3ds/3dslibris/book"},
                   true);
    return haltOnFatalBootStatus();
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "TIMING: scan_books=%llums count=%u",
           (unsigned long long)(osGetTime() - t_scan_ms),
           (unsigned)BookCount());
#endif

  std::sort(books.begin(), books.end(), &book_title_lessthan);

  prefs->Read();
  NormalizeRuntimeAssetPaths(this);
  drawBootStatus("Booting", {"Preparing library..."}, false);
  // Apply key mapping/orientation loaded from prefs.
  SetOrientation(orientation);
  DBG_LOG(this, "Preparing library...");
#ifdef DSLIBRIS_DEBUG
  u64 t_prepare_ms = osGetTime();
#endif
  for (auto &book : books) {
    book->GetBookmarks()->sort();
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "TIMING: prepare_library=%llums",
           (unsigned long long)(osGetTime() - t_prepare_ms));
#endif
  DBG_LOG(this, "Library ready.");

  // Set up menus.
  PrefsInit();
  browser_init();
  browser_.view_dirty = true;

  DBG_LOG(this, VERSION);

  // Resume reading from the last session.
  if (reopen && bookcurrent_) {
    browser_.selected_book = bookcurrent_;
    const char *title = bookcurrent_->GetTitle();
    drawBootStatus("Booting",
                   {"Opening last book...",
                    (title && *title) ? title : "(untitled)"},
                   false);
    OpenBook();
  }

  // Main loop - 3DS style
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    // Keep reading mode responsive: avoid running heavy background jobs while
    // the user is paging through a book.
    if (mode_ != AppMode::Book && mode_ != AppMode::Opening)
      ProcessJobs(3); // Cooperative budget per frame (ms).

    switch (mode_) {
    case AppMode::Book:
      UpdateStatus();
      HandleEventInBook();
      break;

    case AppMode::Opening:
      UpdateStatus();
      HandleEventInOpening();
      break;

    case AppMode::Browser:
      browser_handleevent();
      if (browser_.view_dirty)
        browser_draw();
      break;

    case AppMode::Quit:
      prefs->Write();
      return 0;

    case AppMode::Prefs:
      PrefsHandleEvent();
      if (prefs_view_.view_dirty)
        PrefsDraw();
      break;

    case AppMode::PrefsFont:
    case AppMode::PrefsFontBold:
    case AppMode::PrefsFontItalic:
    case AppMode::PrefsFontBoldItalic:
      fontmenu->handleInput();
      if (fontmenu->isDirty())
        fontmenu->draw();
      break;

    case AppMode::Bookmarks:
      bookmarkmenu->HandleInput(hidKeysDown());
      if (bookmarkmenu->IsDirty())
        bookmarkmenu->Draw();
      break;

    case AppMode::Chapters:
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

  if (scan_with_posix_fallback(bookdir) == 0)
    return 0;

  return 1;
}

// 3DS touch input — map physical touch to our logical buffer coordinates.
// The transform must be the inverse of Text::BlitToFramebuffer() for the
// currently active orientation.
touchPosition App::TouchRead() {
  touchPosition raw;
  hidTouchRead(&raw);
  touchPosition mapped;

  if (!orientation) {
    // Default "Turned Left" orientation (historical mapping), X un-mirrored.
    mapped.px = raw.py;       // -> sx
    mapped.py = 319 - raw.px; // -> sy
  } else {
    // "Turned Right" orientation (opposite page rotation), X un-mirrored.
    mapped.px = 239 - raw.py; // -> sx
    mapped.py = raw.px;       // -> sy
  }

  mapped.px = (u16)std::max(0, std::min(239, (int)mapped.px));
  mapped.py = (u16)std::max(0, std::min(319, (int)mapped.py));

#if ORIENTATION_DIAG
  if (g_orientation_touch_diag_budget > 0) {
    char dmsg[160];
    snprintf(dmsg, sizeof(dmsg),
             "ORIENT touch raw=(%u,%u) mapped=(%u,%u) turned_right=%d",
             (unsigned)raw.px, (unsigned)raw.py, (unsigned)mapped.px,
             (unsigned)mapped.py, orientation ? 1 : 0);
    DBG_LOG(this, dmsg);
    g_orientation_touch_diag_budget--;
  }
#endif

  return mapped;
}

void App::DrawBottomGradientBackground() {
  if (!ts || !ts->screenright)
    return;

  const int w = ts->display.width;       // 240
  const int stride = ts->display.height; // 400 (software page stride)
  const int h = 320;                     // bottom screen logical height
  if (w <= 0 || stride <= 0)
    return;

  static std::vector<u16> gradient;
  static int cachedW = 0;
  static int cachedH = 0;

  if (gradient.empty() || cachedW != w || cachedH != h) {
    gradient.resize((size_t)w * (size_t)h);
    cachedW = w;
    cachedH = h;
    static const u8 kBayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };

    for (int y = 0; y < h; y++) {
      const float tY = (h > 1) ? ((float)y / (float)(h - 1)) : 0.0f;
      for (int x = 0; x < w; x++) {
        const float dx =
            (w > 1)
                ? (((float)x - (float)(w - 1) * 0.5f) / ((float)(w - 1) * 0.5f))
                : 0.0f;
        const float edge = fabsf(dx);

        float r = 244.0f + (238.0f - 244.0f) * tY;
        float g = 226.0f + (220.0f - 226.0f) * tY;
        float b = 195.0f + (185.0f - 195.0f) * tY;

        const float vignette = 1.0f - 0.12f * powf(edge, 1.8f);

        // Ordered dithering (Bayer 4x4) is the primary anti-banding signal
        // for RGB565; tiny stable grain helps hide residual steps.
        const float bayer = (((float)kBayer4x4[y & 3][x & 3] + 0.5f) / 16.0f) -
                            0.5f; // about [-0.47, +0.47]

        const u32 h0 = (u32)x * 73856093u;
        const u32 h1 = (u32)y * 19349663u;
        const u32 h2 = (h0 ^ h1 ^ 0x9E3779B9u);
        const float noise = ((((h2 >> 8) & 0xFF) / 255.0f) - 0.5f) * 0.6f;

        r = r * vignette + bayer * 3.8f + noise;
        g = g * vignette + bayer * 1.9f + noise * 0.6f;
        b = b * vignette + bayer * 3.8f + noise;

        gradient[(size_t)y * (size_t)w + (size_t)x] = RGB565FromU8(r, g, b);
      }
    }
  }

  for (int y = 0; y < h; y++) {
    u16 *dst = ts->screenright + (size_t)y * (size_t)stride;
    const u16 *src = gradient.data() + (size_t)y * (size_t)w;
    memcpy(dst, src, (size_t)w * sizeof(u16));
  }
}

void App::ShowFontView(AppMode app_font_mode) {
  mode_ = AppMode::PrefsFont;
  ts->SetScreen(ts->screenright);
  fontmenu->Open(app_font_mode);
}

void App::ShowLibraryView() {
  // Reset shared bottom buttons immediately; prefs view reuses/moves them.
  buttonprev.Move(2, 296);
  buttonprev.Resize(66, 22);
  buttonprev.Label("prev");
  buttonnext.Move(172, 296);
  buttonnext.Resize(66, 22);
  buttonnext.Label("next");
  buttonprefs.Move(72, 296);
  buttonprefs.Resize(96, 22);
  buttonprefs.Label("settings");
  mode_ = AppMode::Browser;
  ts->SetScreen(ts->screenright);
  browser_.wait_input_release = true;
  browser_.view_dirty = true;
  prefs_view_.layout_notice_pending = false;
}

void App::ShowSettingsView(bool from_book) {
  prefs_view_.from_book = from_book;
  // Surface the warning only when the current book was paginated with stale
  // layout or per-book render settings.
  prefs_view_.layout_notice_pending =
      from_book && bookcurrent_ && BookNeedsRelayout(bookcurrent_);
  PrefsRefreshButton(PREFS_BUTTON_INDEX);
  PrefsRefreshButton(PREFS_BUTTON_BOOKMARKS);
  u8 visible_count = PrefsVisibleButtonCount();
  if (visible_count == 0)
    visible_count = 1;
  if (prefs_view_.selected_index >= visible_count)
    prefs_view_.selected_index = visible_count - 1;
  mode_ = AppMode::Prefs;
  buttonprefs.Label("library");
  ts->SetScreen(ts->screenright);
  prefs_view_.view_dirty = true;
}

void App::MarkBookLayoutDirty() {
  // Bump the global layout generation so already-paginated books are reopened
  // before they are reused.
  layout_revision++;
  if (layout_revision == 0)
    layout_revision = 1;
  prefs_view_.view_dirty = true;
  if (prefs_view_.from_book && bookcurrent_ && bookcurrent_->GetPageCount() > 0)
    prefs_view_.layout_notice_pending = true;
}

bool App::BookNeedsRelayout(Book *book) const {
  if (!book || !book->UsesTextLayoutSettings())
    return false;
  return book && app_flow_utils::NeedsBookRelayout(
                     book->GetPageCount(), book->GetLayoutRevision(),
                     layout_revision, book->NeedsMobiRenderRefresh());
}

void App::ShowBookmarksView() {
  mode_ = AppMode::Bookmarks;
  ts->SetScreen(ts->screenright);
  bookmarkmenu->Init();
}

void App::ShowChaptersView() {
  Book *book = bookcurrent_;
  app_flow_utils::BookFileFormat format =
      app_flow_utils::BookFileFormat::Unsupported;
  bool toc_quality_known = false;
  if (book) {
    format = (book->format == FORMAT_EPUB)
                 ? app_flow_utils::BookFileFormat::Epub
                 : (book->format == FORMAT_PDF)
                       ? app_flow_utils::BookFileFormat::MuPdf
                       : (book->format == FORMAT_CBZ)
                             ? app_flow_utils::BookFileFormat::Cbz
                       : app_flow_utils::BookFileFormat::XhtmlLike;
    toc_quality_known = book->GetTocQuality() != TOC_QUALITY_UNKNOWN;
  }
  app_flow_utils::ChaptersViewDecision decision =
      app_flow_utils::DecideChaptersView(
          book != NULL, format, toc_quality_known,
          book ? book->tocResolveTried : false,
          book ? book->GetChapters().size() : 0);
  if (decision.queue_toc_resolve && book)
    QueueTocResolve(book);
  if (!decision.open_chapters) {
    if (decision.reason == app_flow_utils::ChaptersViewReason::NoCurrentBook) {
      PrintStatus("Index unavailable: no selected book");
    } else {
      PrintStatus("Index unavailable: no chapters");
    }
    ShowSettingsView(true);
    return;
  }
  if (!book || book->GetChapters().empty()) {
    PrintStatus("Index unavailable: no chapters");
    ShowSettingsView(true);
    return;
  }
  mode_ = AppMode::Chapters;
  ts->SetScreen(ts->screenright);
  chaptermenu->Init();
}

void App::ShowCurrentBookView() {
  if (!bookcurrent_)
    return;
  mode_ = AppMode::Book;
  ts->SetScreen(ts->screenright);
}

void App::RequestStatusRedraw() { status_.force_redraw = true; }

void App::UpdateStatus() {
  if (mode_ != AppMode::Book && mode_ != AppMode::Opening)
    return;
  u16 *screen = ts->GetScreen();
  time_t unixTime = time(NULL);
  struct tm *timeStruct = localtime(&unixTime);
  int minute_of_day = -1;
  if (timeStruct) {
    minute_of_day = timeStruct->tm_hour * 60 + timeStruct->tm_min;
  }

  app_flow_utils::StatusSnapshot snapshot = {};
  if (mode_ == AppMode::Book) {
    snapshot = app_flow_utils::ComputeStatusSnapshot(
        {bookcurrent_, bookcurrent_ ? (int)bookcurrent_->GetPosition() : 0,
         bookcurrent_ ? (int)bookcurrent_->GetPageCount() : 0,
         bookcurrent_ ? bookcurrent_->HasDeferredMobiParse() : false,
         status_.progress_lock_book, status_.progress_pagecount_lock});
    status_.progress_lock_book = (Book *)snapshot.next_locked_book;
    status_.progress_pagecount_lock = snapshot.next_locked_pagecount;
  } else {
    status_.progress_lock_book = NULL;
    status_.progress_pagecount_lock = 0;
    snapshot.percent_tenths = -1;
    snapshot.percent_value = 0.0f;
    snapshot.draw_page_count = 0;
    snapshot.has_progress = false;
    snapshot.next_locked_book = NULL;
    snapshot.next_locked_pagecount = 0;
  }

  if (!status_.force_redraw && minute_of_day == status_.last_minute &&
      snapshot.percent_tenths == status_.last_percent_tenths) {
    return;
  }

  char tmsg[24];
  if (!timeStruct) {
    snprintf(tmsg, sizeof(tmsg), "--:--");
  } else if (prefs->time24h) {
    snprintf(tmsg, sizeof(tmsg), "%02d:%02d", timeStruct->tm_hour,
             timeStruct->tm_min);
  } else {
    int h = timeStruct->tm_hour % 12;
    if (h == 0)
      h = 12;
    snprintf(tmsg, sizeof(tmsg), "%02d:%02d %s", h, timeStruct->tm_min,
             timeStruct->tm_hour >= 12 ? "PM" : "AM");
  }

  // Draw on top screen (which is 240x400 in buffer)
  ts->SetScreen(ts->screenleft);
  int style = ts->GetStyle();
  int savedBottomMargin = ts->margin.bottom;
  // Status HUD is outside the text area and should ignore page margins.
  ts->margin.bottom = 0;
  ts->SetStyle(TEXT_STYLE_BROWSER); // smaller, readable font

  u16 fgColor = ts->GetFgColor();

  // Print Clock (Left)
  int textY = 384;
  // Clear a taller band than the nominal footer because the browser font
  // extends several pixels above the baseline and would otherwise leave
  // minute-change artifacts behind.
  const int statusTop = std::max(0, textY - ts->GetHeight() - 8);
  ts->ClearRect(0, (u16)statusTop, 240, 400);
  ts->SetPen(8, textY);
  ts->PrintString(tmsg);
  int clockWidth = ts->GetStringWidth(tmsg, TEXT_STYLE_BROWSER);

  // Print Percentage / opening state (Right)
  int pX = 232;
  if (mode_ == AppMode::Opening) {
    const char *opening_msg = "opening";
    int pw = ts->GetStringWidth(opening_msg, TEXT_STYLE_BROWSER);
    pX = 232 - pw;
    ts->SetPen(pX, textY);
    ts->PrintString(opening_msg);
  } else if (snapshot.has_progress) {
    char pmsg[32];
    snprintf(pmsg, sizeof(pmsg), "%.1f%%", snapshot.percent_value);
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
      if (snapshot.draw_page_count > 1 && bookcurrent_ &&
          bookcurrent_->GetPosition() > 0) {
        int fillW = (int)(((float)(barEnd - barStart - 4) *
                           bookcurrent_->GetPosition()) /
                          (snapshot.draw_page_count - 1));
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
  status_.last_minute = minute_of_day;
  status_.last_percent_tenths = snapshot.percent_tenths;
  status_.force_redraw = false;
}

void App::SetOrientation(bool turned_right) {
  // Keep both input remap and software render orientation in sync.
  orientation = turned_right;
  if (ts) {
    ts->SetOrientation(turned_right);
    ts->MarkAllScreensDirty();
  }
  status_.force_redraw = true;
  browser_.view_dirty = true;
  prefs_view_.view_dirty = true;

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

#if ORIENTATION_DIAG
  g_orientation_touch_diag_budget = 2;
  DBG_LOGF(this, "ORIENT set turned_right=%d", turned_right ? 1 : 0);
#endif
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
