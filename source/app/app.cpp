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
#include "app/library_controller.h"
#include "app/reader_controller.h"
#include "app/settings_controller.h"
#include "app/status_controller.h"
#include "app/startup_controller.h"
#include "app/main_loop_controller.h"
#include "debug_log.h"
#include "path_utils.h"
#include "parse.h"
#include "settings/prefs.h"
#include "ui/text.h"

#ifndef ORIENTATION_DIAG
#define ORIENTATION_DIAG 0
#endif

namespace {} // end anonymous namespace
#include "color_utils.h"

App* App::s_instance_ = nullptr;

App* App::GetInstance() { return s_instance_; }

void App::SetInstance(App* instance) { s_instance_ = instance; }

namespace {

static std::string ResolveDefaultFontDir() {
  return std::string(paths::kFontDir);
}

#if ORIENTATION_DIAG
static int g_orientation_touch_diag_budget = 0;
#endif

} // namespace

App::App() {
  melonds = false;

  fontdir = ResolveDefaultFontDir();
  bookdir = std::string(paths::kBookDir);
  reader_state_.bookcurrent = NULL;
  reopen = true;
  nav_.mode = AppMode::Browser;
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

  nav_.browser.selected_book = NULL;
  nav_.browser.page_start = 0;
  nav_.browser.view_dirty = false;
  nav_.browser.wait_input_release = false;
  nav_.browser.last_interaction_ms = 0;

  prefs = new Prefs(this);
  library_controller_.reset(new LibraryController(*this));
  reader_controller_.reset(new ReaderController(*this));
  settings_controller_.reset(new SettingsController(*this));
  status_controller_.reset(new StatusController(*this));
  startup_controller_.reset(new StartupController(*this));
  main_loop_controller_.reset(new MainLoopController(*this));
  nav_.prefs.selected_index = -1;
  nav_.prefs.view_dirty = false;
  nav_.prefs.from_book = false;
  nav_.prefs.layout_notice_pending = false;
  reader_state_.opening = OpeningState();
  reader_state_.layout_revision = 0;
  reader_state_.pdf_touch_drag_active = false;
  reader_state_.pdf_touch_last_x = -1;
  reader_state_.pdf_touch_last_y = -1;
  reader_state_.pdf_deferred_ready_at_ms = 0;
  reader_state_.mobi_deferred_ready_at_ms = 0;
  status_log_file_ = NULL;
  status_log_write_count_ = 0;
  LightLock_Init(&status_log_lock_);

  ts = new Text();
  ts->app = this;

  fontmenu = new FontMenu(this);
  bookmarkmenu = new BookmarkMenu(this);
  chaptermenu = new ChapterMenu(this);
}

App::~App() {
  LightLock_Lock(&status_log_lock_);
  if (status_log_file_) {
    fflush(status_log_file_);
    fclose(status_log_file_);
    status_log_file_ = NULL;
  }
  LightLock_Unlock(&status_log_lock_);
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

AppMode App::GetMode() const { return nav_.mode; }

void App::SetMode(AppMode mode) { nav_.mode = mode; }

Book *App::GetSelectedBook() const { return nav_.browser.selected_book; }

void App::SetSelectedBook(Book *book) { nav_.browser.selected_book = book; }

Book *App::GetCurrentBook() const { return reader_state_.bookcurrent; }

void App::SetCurrentBook(Book *book) { reader_state_.bookcurrent = book; }

int App::BookCount() const { return (int)books.size(); }

int App::GetSelectedBookIndex() const {
  if (!nav_.browser.selected_book)
    return -1;
  for (size_t i = 0; i < books.size(); i++) {
    if (books[i] == nav_.browser.selected_book)
      return (int)i;
  }
  return -1;
}

int App::GetBrowserPageStart() const { return nav_.browser.page_start; }

void App::SetBrowserPageStart(int page_start) {
  if (page_start < 0)
    page_start = 0;
  nav_.browser.page_start = page_start;
}

u64 App::GetBrowserLastInteractionMs() const {
  return nav_.browser.last_interaction_ms;
}

void App::SetBrowserLastInteractionMs(u64 ms) {
  nav_.browser.last_interaction_ms = ms;
}

bool App::IsBrowserWaitingInputRelease() const {
  return nav_.browser.wait_input_release;
}

void App::SetBrowserWaitingInputRelease(bool wait_input_release) {
  nav_.browser.wait_input_release = wait_input_release;
}

void App::SetBrowserDirty(bool dirty) { nav_.browser.view_dirty = dirty; }

void App::MarkBrowserDirty() { nav_.browser.view_dirty = true; }

int App::GetPrefsSelectedIndex() const { return nav_.prefs.selected_index; }

void App::SetPrefsSelectedIndex(int selected_index) {
  nav_.prefs.selected_index = selected_index;
}

void App::SetBookSettingsContext(bool from_book) {
  nav_.prefs.from_book = from_book;
}

bool App::IsPrefsLayoutNoticePending() const {
  return nav_.prefs.layout_notice_pending;
}

void App::SetPrefsLayoutNoticePending(bool pending) {
  nav_.prefs.layout_notice_pending = pending;
}

void App::SetPrefsDirty(bool dirty) { nav_.prefs.view_dirty = dirty; }

void App::MarkPrefsDirty() { nav_.prefs.view_dirty = true; }

bool App::IsPrefsDirty() const { return nav_.prefs.view_dirty; }

bool App::IsBrowserDirty() const { return nav_.browser.view_dirty; }

void App::PersistPrefs() { prefs->Write(); }

void App::RunFontMenuFrame() {
  fontmenu->handleInput();
  if (fontmenu->isDirty())
    fontmenu->draw();
}

void App::RunBookmarksMenuFrame(u32 keys) {
  bookmarkmenu->HandleInput(keys);
  if (bookmarkmenu->IsDirty())
    bookmarkmenu->Draw();
}

void App::RunChaptersMenuFrame(u32 keys) {
#ifdef DSLIBRIS_DEBUG
  static int s_chapters_frame_budget = 24;
  if (s_chapters_frame_budget > 0) {
    DBG_LOGF(this, "INDEX frame keys=0x%08lx dirty=%d", (unsigned long)keys,
             chaptermenu && chaptermenu->IsDirty() ? 1 : 0);
    s_chapters_frame_budget--;
  }
#endif
  chaptermenu->HandleInput(keys);
  if (chaptermenu->IsDirty())
    chaptermenu->Draw();
}

bool App::PresentIfDirty() {
  if (ts->BlitToFramebuffer()) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    return true;
  }
  return false;
}

int App::StartupFindBooks() { return library_controller_->FindBooks(); }

void App::StartupPrepareLibrary() { library_controller_->PrepareLibrary(); }

void App::StartupInitUiAndBrowser() {
  PrefsInit();
  browser_init();
  SetBrowserDirty(true);
}

void App::StartupInitScreens() { InitScreens(); }

bool App::IsOpeningPending() const { return reader_state_.opening.pending; }

void App::SetOpeningPending(bool pending) { reader_state_.opening.pending = pending; }

Book *App::GetOpeningBook() const { return reader_state_.opening.book; }

void App::SetOpeningBook(Book *book) { reader_state_.opening.book = book; }

bool App::IsOpeningNeedsRelayout() const {
  return reader_state_.opening.needs_relayout;
}

void App::SetOpeningNeedsRelayout(bool needs_relayout) {
  reader_state_.opening.needs_relayout = needs_relayout;
}

int App::GetOpeningOldPageCount() const { return reader_state_.opening.old_page_count; }

void App::SetOpeningOldPageCount(int old_page_count) {
  reader_state_.opening.old_page_count = old_page_count;
}

int App::GetOpeningOldPosition() const { return reader_state_.opening.old_position; }

void App::SetOpeningOldPosition(int old_position) {
  reader_state_.opening.old_position = old_position;
}

std::list<int> &App::MutableOpeningOldBookmarks() {
  return reader_state_.opening.old_bookmarks;
}

u64 App::GetOpeningStartedAtMs() const { return reader_state_.opening.started_at_ms; }

void App::SetOpeningStartedAtMs(u64 started_at_ms) {
  reader_state_.opening.started_at_ms = started_at_ms;
}

bool App::IsDeferredRelayoutPending() const {
  return reader_state_.deferred_relayout.pending;
}

void App::SetDeferredRelayoutPending(bool pending) {
  reader_state_.deferred_relayout.pending = pending;
}

Book *App::GetDeferredRelayoutBook() const {
  return reader_state_.deferred_relayout.book;
}

void App::SetDeferredRelayoutBook(Book *book) {
  reader_state_.deferred_relayout.book = book;
}

int App::GetDeferredRelayoutOldPageCount() const {
  return reader_state_.deferred_relayout.old_page_count;
}

void App::SetDeferredRelayoutOldPageCount(int old_page_count) {
  reader_state_.deferred_relayout.old_page_count = old_page_count;
}

int App::GetDeferredRelayoutOldPosition() const {
  return reader_state_.deferred_relayout.old_position;
}

void App::SetDeferredRelayoutOldPosition(int old_position) {
  reader_state_.deferred_relayout.old_position = old_position;
}

std::list<int> &App::MutableDeferredRelayoutOldBookmarks() {
  return reader_state_.deferred_relayout.old_bookmarks;
}

int App::GetDeferredRelayoutInitialPosition() const {
  return reader_state_.deferred_relayout.initial_position;
}

void App::SetDeferredRelayoutInitialPosition(int initial_position) {
  reader_state_.deferred_relayout.initial_position = initial_position;
}

unsigned int App::GetLayoutRevision() const { return reader_state_.layout_revision; }

void App::SetLayoutRevision(unsigned int layout_revision) {
  reader_state_.layout_revision = layout_revision;
}

bool App::IsPdfTouchDragActive() const { return reader_state_.pdf_touch_drag_active; }

void App::SetPdfTouchDragActive(bool active) {
  reader_state_.pdf_touch_drag_active = active;
}

int App::GetPdfTouchLastX() const { return reader_state_.pdf_touch_last_x; }

void App::SetPdfTouchLastX(int x) { reader_state_.pdf_touch_last_x = x; }

int App::GetPdfTouchLastY() const { return reader_state_.pdf_touch_last_y; }

void App::SetPdfTouchLastY(int y) { reader_state_.pdf_touch_last_y = y; }

u64 App::GetPdfDeferredReadyAtMs() const {
  return reader_state_.pdf_deferred_ready_at_ms;
}

void App::SetPdfDeferredReadyAtMs(u64 ready_at_ms) {
  reader_state_.pdf_deferred_ready_at_ms = ready_at_ms;
}

u64 App::GetMobiDeferredReadyAtMs() const {
  return reader_state_.mobi_deferred_ready_at_ms;
}

void App::SetMobiDeferredReadyAtMs(u64 ready_at_ms) {
  reader_state_.mobi_deferred_ready_at_ms = ready_at_ms;
}

int App::Run(void) {
  const int startup = startup_controller_->RunBootSequence();
  if (startup == 1)
    return 1;
  if (startup == 2)
    return 0;
  return main_loop_controller_->RunMainLoop();
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
  nav_.mode = AppMode::PrefsFont;
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
  nav_.mode = AppMode::Browser;
  ts->SetScreen(ts->screenright);
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  nav_.browser.view_dirty = true;
  nav_.prefs.layout_notice_pending = false;
}

void App::ShowSettingsView(bool from_book) {
  settings_controller_->ShowSettingsView(from_book);
}

void App::MarkBookLayoutDirty() {
  // Bump the global layout generation so already-paginated books are reopened
  // before they are reused.
  reader_state_.layout_revision++;
  if (reader_state_.layout_revision == 0)
    reader_state_.layout_revision = 1;
  nav_.prefs.view_dirty = true;
  if (nav_.prefs.from_book && reader_state_.bookcurrent && reader_state_.bookcurrent->GetPageCount() > 0)
    nav_.prefs.layout_notice_pending = true;
}

bool App::BookNeedsRelayout(Book *book) const {
  if (!book || !book->UsesTextLayoutSettings())
    return false;
  return book && app_flow_utils::NeedsBookRelayout(
                     book->GetPageCount(), book->GetLayoutRevision(),
                     reader_state_.layout_revision, book->NeedsMobiRenderRefresh());
}

void App::ShowBookmarksView() {
  nav_.mode = AppMode::Bookmarks;
  ts->SetScreen(ts->screenright);
  bookmarkmenu->Init();
}

void App::ShowChaptersView() {
  DBG_LOG(this, "INDEX show begin");
  Book *book = reader_state_.bookcurrent;
  format_t format = FORMAT_UNDEF;
  bool toc_quality_known = false;
  if (book) {
    format = book->format;
    toc_quality_known = book->GetTocQuality() != TOC_QUALITY_UNKNOWN;
    DBG_LOGF(this,
             "INDEX request mode=%d book=%p fmt=%d chapters=%u tocq=%d tried=%d",
             (int)nav_.mode, (void *)book, (int)format,
             (unsigned)book->GetChapters().size(), (int)book->GetTocQuality(),
             book->tocResolveTried ? 1 : 0);
  } else {
    DBG_LOGF(this, "INDEX request mode=%d book=null", (int)nav_.mode);
  }
  app_flow_utils::ChaptersViewDecision decision =
      app_flow_utils::DecideChaptersView(
          book != NULL, format, toc_quality_known,
          book ? book->tocResolveTried : false,
          book ? book->GetChapters().size() : 0);
  DBG_LOGF(this, "INDEX decision open=%d queue=%d reason=%d",
           decision.open_chapters ? 1 : 0, decision.queue_toc_resolve ? 1 : 0,
           (int)decision.reason);
  // Opening index must stay responsive. If chapters already exist, skip
  // deferred TOC resolve here and use the available chapter list.
  if (decision.queue_toc_resolve && book &&
      book->GetChapters().empty())
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
  nav_.mode = AppMode::Chapters;
  ts->SetScreen(ts->screenright);
  DBG_LOG(this, "INDEX show init menu begin");
  chaptermenu->Init();
  DBG_LOG(this, "INDEX show init menu end");
  DBG_LOGF(this, "INDEX open chapters=%u page_count=%u",
           (unsigned)book->GetChapters().size(), (unsigned)book->GetPageCount());
}

void App::ShowCurrentBookView() {
  if (!reader_state_.bookcurrent)
    return;
  nav_.mode = AppMode::Book;
  ts->SetScreen(ts->screenright);
}

void App::RequestStatusRedraw() { status_controller_->RequestStatusRedraw(); }

void App::UpdateStatus() { status_controller_->UpdateStatus(); }

void App::SetOrientation(bool turned_right) {
  // Keep both input remap and software render orientation in sync.
  orientation = turned_right;
  if (ts) {
    ts->SetOrientation(turned_right);
    ts->MarkAllScreensDirty();
  }
  RequestStatusRedraw();
  nav_.browser.view_dirty = true;
  nav_.prefs.view_dirty = true;

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
  if (!msg)
    return;

  LightLock_Lock(&status_log_lock_);

  if (!status_log_file_) {
    status_log_file_ = fopen(paths::kLogFile, "a");
    if (status_log_file_)
      setvbuf(status_log_file_, NULL, _IOFBF, 4096);
  }

  if (status_log_file_) {
    time_t rawtime;
    struct tm *info;
    char buffer[80];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", info);

    fprintf(status_log_file_, "[%s] %s\n", buffer, msg);
    status_log_write_count_++;
#ifdef DSLIBRIS_DEBUG
    fflush(status_log_file_);
#else
    if ((status_log_write_count_ & 15u) == 0u)
      fflush(status_log_file_);
#endif
  }

  LightLock_Unlock(&status_log_lock_);
}

void App::PrintStatus(std::string msg) { PrintStatus(msg.c_str()); }
