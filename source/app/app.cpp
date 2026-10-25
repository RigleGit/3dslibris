#include "shared/fixed_layout_perf.h"
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
#include "book/book_renderer.h"
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
#include "shared/boot_trace.h"
#include "shared/debug_log.h"
#include "shared/orientation_utils.h"
#include "shared/touch_map_utils.h"
#include "shared/path_constants.h"
#include "parse.h"
#include "shared/debug_runtime_mode.h"
#include "settings/prefs.h"
#include "reader/book_switch_utils.h"
#include "ui/text.h"
#include "shared/screen_dimensions.h"
#include "ui/screen_layout_constants.h"

#ifndef ORIENTATION_DIAG
#define ORIENTATION_DIAG 0
#endif

#include "ui/gradient_utils.h"

// Singleton instance management for App class, allowing global access to the app instance from other modules.
App *App::s_instance_ = nullptr;

App *App::GetInstance() { return s_instance_; }
void App::SetInstance(App *instance) { s_instance_ = instance; }

namespace
{

  static std::string ResolveDefaultFontDir()
  {
    return paths::GetFontDir();
  }

#if ORIENTATION_DIAG
  static int g_orientation_touch_diag_budget = 0;
#endif

  [[gnu::unused]] static const char *AppletHookName(APT_HookType hook)
  {
    switch (hook)
    {
    case APTHOOK_ONSUSPEND:
      return "suspend";
    case APTHOOK_ONRESTORE:
      return "restore";
    case APTHOOK_ONWAKEUP:
      return "wakeup";
    case APTHOOK_ONEXIT:
      return "exit";
    default:
      return "unknown";
    }
  }

} // namespace

App::App()
{
  // Initialize paths and state.
  fontdir = ResolveDefaultFontDir();
  bookdir = paths::GetBookDir();
  reader_state_.bookcurrent = nullptr;
  reopen = true; // Reopen last book on startup by default.
  nav_.mode = AppMode::Browser;
  cache = false;
  portrait_orientation = orientation_utils::ORIENT_TURNED_LEFT;
  render_orientation = orientation_utils::ORIENT_TURNED_LEFT;
  landscape = false;
  colorMode = 0;

  // Default key mappings
  // Circle Pad.
  key.up = KEY_CPAD_UP;
  key.down = KEY_CPAD_DOWN;
  key.left = KEY_CPAD_LEFT;
  key.right = KEY_CPAD_RIGHT;

  // D-pad.
  key.dup = KEY_DUP;
  key.ddown = KEY_DDOWN;
  key.dleft = KEY_DLEFT;
  key.dright = KEY_DRIGHT;

  // Face buttons.
  key.a = KEY_A;
  key.b = KEY_B;
  key.x = KEY_X;
  key.y = KEY_Y;

  // System buttons.
  key.start = KEY_START;
  key.select = KEY_SELECT;

  // Shoulders.
  key.l = KEY_L;
  key.r = KEY_R;
  key.zl = KEY_ZL;
  key.zr = KEY_ZR;
  key.downrepeat = key.down | key.ddown;

  // TODO: add new3ds-specific keys (c-stick) to prefs and remappable key config.

  // Initialize browser navigation state.
  nav_.browser.selected_book = nullptr;
  nav_.browser.page_start = 0;
  nav_.browser.view_dirty = false;
  nav_.browser.wait_input_release = false;
  nav_.browser.last_interaction_ms = 0;

  // Initialize controllers and other components.
  prefs = std::unique_ptr<Prefs>(new Prefs(this));
  library_controller_ = std::unique_ptr<LibraryController>(new LibraryController(*this));
  reader_controller_ = std::unique_ptr<ReaderController>(new ReaderController(*this));
  settings_controller_ = std::unique_ptr<SettingsController>(new SettingsController(*this));
  status_controller_ = std::unique_ptr<StatusController>(new StatusController(*this));
  startup_controller_ = std::unique_ptr<StartupController>(new StartupController(*this));
  main_loop_controller_ = std::unique_ptr<MainLoopController>(new MainLoopController(*this));

  // Initialize prefs view state.
  nav_.prefs.selected_index = -1;
  nav_.prefs.view_dirty = false;
  nav_.prefs.from_book = false;
  nav_.prefs.layout_notice_pending = false;

  // Initialize reader runtime state.
  reader_state_.opening = OpeningState();
  reader_state_.layout_revision = 0;
  reader_state_.pdf_touch_drag_active = false;
  reader_state_.pdf_touch_last_x = -1;
  reader_state_.pdf_touch_last_y = -1;
  reader_state_.pdf_deferred_ready_at_ms = 0;

  // Initialize status log.
  status_log_file_ = nullptr;
  status_log_write_count_ = 0;
  LightLock_Init(&status_log_lock_); // Protects status log file access.

  // Initialize 3DS-specific state and hooks.
  pending_boot_reopen_ = false;
  skip_next_browser_present_ = false;

  bool new3ds = false;
  APT_CheckNew3DS(&new3ds);
  lifecycle_state_.SetNew3DS(new3ds);
  lifecycle_state_.SetHomebrew(envIsHomebrew());
  lifecycle_state_.InstallHook(App::AptHookCallback, this);


#ifdef DSLIBRIS_DEBUG
  // Debug builds should emit actionable logs by default.
  debug_log::SetLevel(DBG_LEVEL_DEBUG);
  debug_log::SetCategories(DBG_CAT_ALL);
#endif

  // Initialize UI components.
  ts = std::unique_ptr<Text>(new Text());
  ts->SetReporter(this);
  ts->SetFontDir(fontdir);

  fontmenu = std::unique_ptr<FontMenu>(new FontMenu(this));
  bookmarkmenu = std::unique_ptr<BookmarkMenu>(new BookmarkMenu(this));
  chaptermenu = std::unique_ptr<ChapterMenu>(new ChapterMenu(this));

#ifdef DSLIBRIS_DEBUG
  // Log environment details for debugging purposes.
  DBG_LOGF(this, "ENV runtime=%s device=%s",
           lifecycle_state_.IsHomebrew() ? "3dsx/homebrew" : "cia/title",
           lifecycle_state_.IsNew3DS() ? "new3ds" : "old3ds");
  if (debug_runtime::BackgroundWorkersDisabled())
  {
    DBG_LOG(this, "SAFE mode: background workers disabled");
  }
  if (debug_runtime::BrowserWarmupDisabled())
  {
    DBG_LOG(this, "BROWSER warmup: disabled");
  }
#endif
}

App::~App()
{
  PrepareForShutdown();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: aptUnhook begin");
#endif
  lifecycle_state_.UninstallHook();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: start");
#endif
  LightLock_Lock(&status_log_lock_); // Ensure exclusive access to status log during cleanup.
  if (status_log_file_)
  {
    fflush(status_log_file_);
    fclose(status_log_file_);
    status_log_file_ = nullptr;
  }
  LightLock_Unlock(&status_log_lock_);
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: deleting books");
#endif
  // Delete all book instances to free resources.
  for (std::vector<Book *>::iterator it = books.begin(); it != books.end();
       it++)
    delete *it;
  books.clear();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: deleting buttons");
#endif
  // Delete all UI buttons.
  for (size_t i = 0; i < buttons.size(); i++)
    delete buttons[i];
  buttons.clear();
#ifdef DSLIBRIS_DEBUG
  PrintStatus("APP ~App: done");
#endif
  UiButtonSkin_Exit(); // Clean up button skin resources.
}

// NOTE: nav/mode/selected/current/browser/prefs accessors moved to
// app_accessors.cpp. Menu frame entry points moved to app_menu_frames.cpp.

bool App::PresentIfDirty()
{
  if (lifecycle_state_.IsSuspended())
    return false;
  const bool measure_present = fixed_perf::NeedsPresentationTiming();
  uint64_t perf_phase = measure_present ? fixed_perf::Now() : 0;
  const bool had_dirty = ts->HasDirtyScreens();
  const bool wrote = ts->BlitToFramebuffer();
  if (measure_present)
    fixed_perf::ViewStage("present.framebuffer_copy",
                          fixed_perf::Now() - perf_phase, wrote ? 1 : 0);
  const bool browser_idle_copy = (nav_.mode == AppMode::Browser && !had_dirty && wrote);
  if (browser_idle_copy)
    return false;
  if (wrote)
  {
    if (measure_present)
      perf_phase = fixed_perf::Now();
    gfxFlushBuffers();
    if (measure_present) {
      fixed_perf::ViewStage("present.flush", fixed_perf::Now() - perf_phase);
      perf_phase = fixed_perf::Now();
    }
    gfxSwapBuffers();
    if (measure_present)
      fixed_perf::ViewStage("present.swap", fixed_perf::Now() - perf_phase);
    fixed_perf::Presented();
    return true;
  }
  return false;
}

// NOTE: trivial accessors and small forwarders moved to app_accessors.cpp.

// NOTE: applet lifecycle methods (PrepareForShutdown, AptHookCallback,
// HandleAppletHook, HandleAppletSuspend, HandleAppletResume) moved to
// app_lifecycle.cpp.

// Main app run loop: execute startup sequence and then enter the main loop controller.
int App::Run(void)
{
  boot_trace::Boot("app run begin");
  const int startup = startup_controller_->RunBootSequence();
  if (startup == 1) {
    boot_trace::Boot("app startup failed");
    return 1;
  }
  if (startup == 2) {
    boot_trace::Boot("app startup halted");
    return 0;
  }
  boot_trace::Boot("app startup complete");
  boot_trace::Boot("app main loop begin");
  return main_loop_controller_->RunMainLoop();
}

// 3DS touch input — map physical touch to our logical buffer coordinates.
// The transform must be the inverse of Text::BlitToFramebuffer() for the
// currently active orientation.
touchPosition App::MapTouch(const FrameInput &input) const
{
  const touch_map_utils::TouchPoint point = input.MapTouch(render_orientation);
  touchPosition mapped;
  mapped.px = (u16)point.x;
  mapped.py = (u16)point.y;

#if ORIENTATION_DIAG
  if (g_orientation_touch_diag_budget > 0)
  {
    char dmsg[160];
    snprintf(dmsg, sizeof(dmsg),
             "ORIENT touch raw=(%u,%u) mapped=(%u,%u) turned_right=%d",
             (unsigned)input.touch_raw_x, (unsigned)input.touch_raw_y,
             (unsigned)mapped.px,
             (unsigned)mapped.py, reader_layout.orientation ? 1 : 0);
    DBG_LOG(this, dmsg);
    g_orientation_touch_diag_budget--;
  }
#endif

  return mapped;
}

void App::DrawBottomGradientBackground()
{
  gradient_utils::DrawToScreen(ts.get(), colorMode, ts->screenright,
                               ts->LogicalHeightFor(false));
}

void App::DrawTopGradientBackground()
{
  gradient_utils::DrawToScreen(ts.get(), colorMode, ts->screenleft,
                               ts->LogicalHeightFor(true));
}

// Show the font selection menu, initializing it with the specified font mode (regular, bold, italic, etc.).
// NOTE: Show*View / ReturnFromPrefs / MarkBookLayoutDirty /
// BookNeedsRelayout moved to app_views.cpp.

void App::RequestStatusRedraw() { status_controller_->RequestStatusRedraw(); }

void App::UpdateStatus() { status_controller_->UpdateStatus(); }

// Set the screen orientation (turned left/right or landscape) and update touch
// input mapping, button layout, and mark screens dirty for redraw.
void App::SetOrientation(u8 new_orientation)
{
  if (orientation_utils::IsLandscape(new_orientation)) {
    landscape = true;
  } else {
    landscape = false;
    portrait_orientation = orientation_utils::IsTurnedRight(new_orientation)
                               ? orientation_utils::ORIENT_TURNED_RIGHT
                               : orientation_utils::ORIENT_TURNED_LEFT;
  }
  reader_layout.orientation = landscape ? orientation_utils::ORIENT_LANDSCAPE
                          : portrait_orientation;
  const bool reading_view = nav_.mode == AppMode::Book;
  ApplyRenderOrientation(reading_view ? reader_layout.orientation : portrait_orientation);
}

void App::SetHandedness(u8 portrait_orientation_value)
{
  portrait_orientation =
      orientation_utils::IsTurnedRight(portrait_orientation_value)
          ? orientation_utils::ORIENT_TURNED_RIGHT
          : orientation_utils::ORIENT_TURNED_LEFT;
  if (!landscape)
    reader_layout.orientation = portrait_orientation;
  const bool reading_view = nav_.mode == AppMode::Book;
  ApplyRenderOrientation(reading_view ? reader_layout.orientation : portrait_orientation);
}

void App::ApplyRenderOrientation(u8 new_orientation)
{
  const bool turned_right =
      orientation_utils::IsTurnedRight(portrait_orientation);
  render_orientation = new_orientation;
  if (ts)
  {
    ts->SetOrientation(new_orientation);
    ts->MarkAllScreensDirty();
  }
  RequestStatusRedraw();
  nav_.browser.view_dirty = true;
  nav_.prefs.view_dirty = true;

  key.up = KEY_CPAD_UP;
  key.down = KEY_CPAD_DOWN;
  key.left = KEY_CPAD_LEFT;
  key.right = KEY_CPAD_RIGHT;
  key.dup = KEY_DUP;
  key.ddown = KEY_DDOWN;
  key.dleft = KEY_DLEFT;
  key.dright = KEY_DRIGHT;
  key.l = turned_right ? KEY_R : KEY_L;
  key.r = turned_right ? KEY_L : KEY_R;
  key.zl = turned_right ? KEY_ZR : KEY_ZL;
  key.zr = turned_right ? KEY_ZL : KEY_ZR;
  key.downrepeat = key.down | key.ddown;

#if ORIENTATION_DIAG
  g_orientation_touch_diag_budget = 2;
  DBG_LOGF(this, "ORIENT set turned_right=%d", turned_right ? 1 : 0);
#endif
}

// Initialize the top and bottom screens with double buffering and the correct pixel format, then clear the software buffers for both screens.
void App::InitScreens()
{
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

void App::PrintStatus(const char *msg)
{
  if (!msg)
    return;

  LightLock_Lock(&status_log_lock_);

  if (!status_log_file_)
  {
    status_log_file_ = fopen(paths::GetLogFile().c_str(), "a");
    if (status_log_file_)
      setvbuf(status_log_file_, NULL, _IOFBF, 4096);
  }

  if (status_log_file_)
  {
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
