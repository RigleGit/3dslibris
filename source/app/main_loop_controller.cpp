#include "shared/fixed_layout_perf.h"
/*
    3dslibris - main_loop_controller.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Main loop controller for the 3DS port, driving the per-frame app update cycle.
    - Dispatches behavior by AppMode, including browser, reader, opening, prefs, and menus.
    - Handles applet suspend/resume flow, pending boot reopen, and browser warmup/background jobs.
    - Presents updated frames only when the UI has dirty content.
*/

#include "app/main_loop_controller.h"

#include <3ds.h>
#include <string>

#include "app/app.h"
#include "book/book.h"
#include "shared/boot_trace.h"
#include "shared/debug_log.h"
#include "library/browser_warmup_utils.h"
#include "shared/debug_runtime_mode.h"
#include "settings/prefs.h"

MainLoopController::MainLoopController(App &app) : app_(app) {}

int MainLoopController::RunMainLoop()
{
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(&app_, "PERF schema=1 new3ds=%d homebrew=%d units=us pages=1-based memory=bytes",
           app_.IsNew3dsDevice() ? 1 : 0, app_.IsHomebrewEnvironment() ? 1 : 0);
  AppMode last_mode = app_.GetMode();
  int mode_log_budget = 64;
  int heap_log_countdown = 0;
  int lifecycle_log_budget = 32;
#endif
  auto handle_lifecycle_pause = [&]() -> bool {
    boot_trace::FlushAptHooks();
    if (app_.IsAppletExitRequested())
    {
      boot_trace::Boot("main loop exit requested");
      app_.SetMode(AppMode::Quit);
      return false;
    }
    if (app_.GetMode() == AppMode::Quit)
      return false;
    if (app_.IsAppletSuspended() || !aptIsActive())
    {
      boot_trace::Boot("main loop suspend begin");
#ifdef DSLIBRIS_DEBUG
      if (lifecycle_log_budget > 0)
      {
        DBG_LOGF(&app_, "MAIN lifecycle pause before frame mode=%d",
                 (int)app_.GetMode());
        lifecycle_log_budget--;
      }
#endif
      app_.HandleAppletSuspend();
      boot_trace::Boot("main loop suspend handled");
      svcSleepThread(1000000LL); // 1ms yield: aptMainLoop() may return briefly while HOME transitions
      return true;
    }
    app_.HandleAppletResume();
    boot_trace::FlushAptHooks();
    return false;
  };

  while (aptMainLoop())
  {
    if (app_.GetMode() != AppMode::Book)
      fixed_perf::ResetView();
    if (app_.GetMode() == AppMode::Quit)
    {
      // main() calls PrepareForShutdown, which owns the final save.
      return 0;
    }

    // Main loop runs until aptMainLoop returns false (app exit). Handle APT
    // suspend before touching GSP/HID so HOME transitions do not run another
    // frame of graphics/input work after the hook requests suspension.
    if (handle_lifecycle_pause())
      continue;

#ifdef DSLIBRIS_DEBUG
    if (lifecycle_log_budget > 0)
    {
      DBG_LOGF(&app_, "MAIN before vblank mode=%d", (int)app_.GetMode());
      lifecycle_log_budget--;
    }
#endif
    gspWaitForVBlank(); // Sync with display refresh to avoid tearing and control frame timing.
#ifdef DSLIBRIS_DEBUG
    if (lifecycle_log_budget > 0)
    {
      DBG_LOGF(&app_, "MAIN after vblank mode=%d suspended=%u",
               (int)app_.GetMode(), app_.IsAppletSuspended() ? 1u : 0u);
      lifecycle_log_budget--;
    }
#endif
    if (handle_lifecycle_pause())
      continue;

#ifdef DSLIBRIS_DEBUG
    if (lifecycle_log_budget > 0)
    {
      DBG_LOGF(&app_, "MAIN before hid mode=%d", (int)app_.GetMode());
      lifecycle_log_budget--;
    }
#endif
    hidScanInput(); // Update input state for this frame, must be called before reading keys.
    touchPosition raw_touch = {};
    hidTouchRead(&raw_touch);
    const u32 frame_keys_down = hidKeysDown();
    const u32 frame_keys_held = hidKeysHeld();
    const FrameInput input(frame_keys_down, frame_keys_held,
                           (frame_keys_held & KEY_TOUCH) != 0,
                           (int)raw_touch.px, (int)raw_touch.py, osGetTime());
#ifdef DSLIBRIS_DEBUG
    if (lifecycle_log_budget > 0)
    {
      DBG_LOGF(&app_, "MAIN after hid mode=%d suspended=%u",
               (int)app_.GetMode(), app_.IsAppletSuspended() ? 1u : 0u);
      lifecycle_log_budget--;
    }
#endif
    if (handle_lifecycle_pause())
      continue;

    if (app_.HasPendingBootReopen())
    {
      boot_trace::Boot("main pending boot reopen begin");
      app_.SetPendingBootReopen(false);
      app_.OpenBook();
      boot_trace::Boot("main pending boot reopen done");
    }

#ifdef DSLIBRIS_DEBUG
    if (mode_log_budget > 0 && app_.GetMode() != last_mode)
    {
      app_.PrintStatus("MAIN mode transition");
      app_.PrintStatus(std::string("MAIN mode now=") +
                       std::to_string((int)app_.GetMode()));
      last_mode = app_.GetMode();
      mode_log_budget -= 2;
    }
#endif
    // Dispatch frame processing based on the current app mode, handling input and updates for each mode. After processing, present the frame if it was marked dirty.
    const AppMode input_mode = app_.GetMode();
#ifdef DSLIBRIS_DEBUG
    const uint64_t input_started_ms = osGetTime();
#endif
    switch (app_.GetMode())
    {
    case AppMode::Book:
      app_.UpdateStatus();
      app_.HandleEventInBook(input);
      break;

    case AppMode::Opening:
      app_.UpdateStatus();
      app_.HandleEventInOpening(input);
      break;

    case AppMode::Browser:
      app_.browser_handleevent(input);
      if (app_.GetMode() != AppMode::Browser)
      {
        DBG_LOGF(&app_, "MAIN browser frame aborted after handleevent mode=%d",
                 (int)app_.GetMode());
        break;
      }
      if (!debug_runtime::BrowserWarmupDisabled())
        app_.TickBrowserWarmup();
      if (app_.GetMode() != AppMode::Browser)
      {
        DBG_LOGF(&app_, "MAIN browser frame aborted after warmup mode=%d",
                 (int)app_.GetMode());
        break;
      }
      if (app_.IsBrowserDirty())
        app_.browser_draw();
      else
        app_.browser_tick_marquee();
#ifdef DSLIBRIS_DEBUG
      if (--heap_log_countdown <= 0)
      {
        heap_log_countdown = 300; // ~5 seconds at 60fps
        app_.PrintStatus(std::string("MEM heap_free=") +
                         std::to_string((int)osGetMemRegionFree(MEMREGION_ALL)));
      }
#endif
      break;

    case AppMode::Quit:
      // main() calls PrepareForShutdown, which owns the final save.
      return 0;

    case AppMode::Prefs:
      app_.PrefsHandleEvent(input);
      if (app_.GetMode() != AppMode::Prefs)
        break;
      if (app_.IsPrefsDirty())
        app_.PrefsDraw();
      break;

    case AppMode::PrefsFont:
    case AppMode::PrefsFontBold:
    case AppMode::PrefsFontItalic:
    case AppMode::PrefsFontBoldItalic:
      app_.RunFontMenuFrame(input);
      break;

    case AppMode::Bookmarks:
      app_.RunBookmarksMenuFrame(input);
      break;

    case AppMode::Chapters:
      app_.RunChaptersMenuFrame(input);
      break;

    case AppMode::ImageViewer:
      app_.RunImageViewerFrame(input);
      break;

    case AppMode::Search:
      app_.RunSearchFrame(input);
      break;

    case AppMode::BookInfo:
      app_.RunBookInfoFrame(input);
      break;
    }

    if (app_.GetMode() == AppMode::Quit)
    {
      // main() calls PrepareForShutdown, which owns the final save.
      return 0;
    }

    if (app_.GetMode() == AppMode::Browser && app_.IsBrowserDirty())
      app_.browser_draw();

    if (app_.GetMode() == AppMode::Browser &&
        app_.ShouldSkipNextBrowserPresent() && !app_.IsBrowserDirty() &&
        !app_.ts->HasDirtyScreens())
    {
      app_.ClearSkipNextBrowserPresent();
    }
    else
    {
      app_.PresentIfDirty();
    }
#ifdef DSLIBRIS_DEBUG
    const uint64_t input_elapsed_ms = osGetTime() - input_started_ms;
    if (input_elapsed_ms >= 32)
      DBG_LOGF(&app_, "TIMING: ui_frame mode=%d ms=%llu keys=%lu held=%lu",
               (int)input_mode, (unsigned long long)input_elapsed_ms,
               (unsigned long)input.keys_down, (unsigned long)input.keys_held);
#endif

    // Present the input response before SD writes or non-preemptible jobs.
    const bool leaving_prefs = input_mode == AppMode::Prefs &&
                              app_.GetMode() != AppMode::Prefs;
    bool wrote_prefs = false;
    if (!app_.IsAppletSuspended() && aptIsActive() &&
        (leaving_prefs || (input.keys_down == 0 && input.keys_held == 0)))
      wrote_prefs = app_.prefs->FlushPendingWrite(leaving_prefs);
    if (!wrote_prefs && app_.GetMode() == AppMode::Browser &&
        input.keys_down == 0 && input.keys_held == 0 &&
        !debug_runtime::BrowserWarmupDisabled() &&
        !app_.IsAppletSuspended() && aptIsActive() &&
        browser_warmup_utils::IsBrowserWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease()))
      app_.ProcessJobs(3);
    if (!wrote_prefs && app_.GetMode() == AppMode::Book &&
        input.keys_down == 0 && input.keys_held == 0 &&
        !app_.ShouldAbortWork() && aptIsActive()) {
      Book *book = app_.GetCurrentBook();
      if (book)
        book->PumpPendingEpubPageCacheSave();
    }
    fixed_perf::Flush(&app_);
  }
  fixed_perf::Flush(&app_);
  // PrepareForShutdown persists before releasing the current book.
  return 0;
}
