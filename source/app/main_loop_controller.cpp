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
#include "shared/debug_log.h"
#include "library/browser_warmup_utils.h"
#include "shared/debug_runtime_mode.h"

MainLoopController::MainLoopController(App &app) : app_(app) {}

int MainLoopController::RunMainLoop()
{
#ifdef DSLIBRIS_DEBUG
  AppMode last_mode = app_.GetMode();
  int mode_log_budget = 64;
  int heap_log_countdown = 0;
  int lifecycle_log_budget = 32;
#endif
  auto handle_lifecycle_pause = [&]() -> bool {
    if (app_.GetMode() == AppMode::Quit)
      return false;
    if (app_.IsAppletSuspended())
    {
#ifdef DSLIBRIS_DEBUG
      if (lifecycle_log_budget > 0)
      {
        DBG_LOGF(&app_, "MAIN lifecycle pause before frame mode=%d",
                 (int)app_.GetMode());
        lifecycle_log_budget--;
      }
#endif
      app_.HandleAppletSuspend();
      svcSleepThread(1000000LL); // 1ms yield: aptMainLoop() may return briefly while HOME transitions
      return true;
    }
    app_.HandleAppletResume();
    return false;
  };

  while (aptMainLoop())
  {
    if (app_.GetMode() == AppMode::Quit)
    {
      app_.PersistPrefs();
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
      app_.SetPendingBootReopen(false);
      app_.OpenBook();
    }

    // Allow browser warmup jobs to run during idle periods in the browser, based on timing and input state.
    if (app_.GetMode() == AppMode::Browser)
    {
      if (!debug_runtime::BrowserWarmupDisabled())
      {
        bool allow_jobs = browser_warmup_utils::IsBrowserWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
        if (allow_jobs)
          app_.ProcessJobs(3); // Process background jobs with a small time budget during idle periods to warm up the browser without impacting responsiveness.
      }
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
    switch (app_.GetMode())
    {
    case AppMode::Book:
      app_.UpdateStatus();
      app_.HandleEventInBook();
      break;

    case AppMode::Opening:
      app_.UpdateStatus();
      app_.HandleEventInOpening();
      break;

    case AppMode::Browser:
      app_.browser_handleevent();
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
      app_.PersistPrefs();
      return 0;

    case AppMode::Prefs:
      app_.PrefsHandleEvent();
      if (app_.GetMode() != AppMode::Prefs)
        break;
      if (app_.IsPrefsDirty())
        app_.PrefsDraw();
      break;

    case AppMode::PrefsFont:
    case AppMode::PrefsFontBold:
    case AppMode::PrefsFontItalic:
    case AppMode::PrefsFontBoldItalic:
      app_.RunFontMenuFrame(hidKeysDown());
      break;

    case AppMode::Bookmarks:
      app_.RunBookmarksMenuFrame(hidKeysDown());
      break;

    case AppMode::Chapters:
      app_.RunChaptersMenuFrame(hidKeysDown());
      break;
    }

    if (app_.GetMode() == AppMode::Quit)
    {
      app_.PersistPrefs();
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
  }
  return 0;
}
