#include "app/main_loop_controller.h"

#include <3ds.h>
#include <string>

#include "app/app.h"
#include "library/browser_warmup_utils.h"

MainLoopController::MainLoopController(App &app) : app_(app) {}

int MainLoopController::RunMainLoop() {
#ifdef DSLIBRIS_DEBUG
  AppMode last_mode = app_.GetMode();
  int mode_log_budget = 64;
  int heap_log_countdown = 0;
#endif
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();

    if (app_.IsAppletSuspended()) {
      app_.HandleAppletSuspend();
      continue;
    }
    app_.HandleAppletResume();

    if (app_.HasPendingBootReopen()) {
      app_.SetPendingBootReopen(false);
      app_.OpenBook();
    }

    if (app_.GetMode() == AppMode::Browser) {
      bool allow_jobs = browser_warmup_utils::IsBrowserWarmupIdle(
          osGetTime(), app_.GetBrowserLastInteractionMs(),
          app_.IsBrowserWaitingInputRelease());
      if (allow_jobs)
        app_.ProcessJobs(3);
    }

#ifdef DSLIBRIS_DEBUG
    if (mode_log_budget > 0 && app_.GetMode() != last_mode) {
      app_.PrintStatus("MAIN mode transition");
      app_.PrintStatus(std::string("MAIN mode now=") +
                       std::to_string((int)app_.GetMode()));
      last_mode = app_.GetMode();
      mode_log_budget -= 2;
    }
#endif

    switch (app_.GetMode()) {
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
      app_.TickBrowserWarmup();
      app_.browser_tick_marquee();
      if (app_.IsBrowserDirty())
        app_.browser_draw();
#ifdef DSLIBRIS_DEBUG
      if (--heap_log_countdown <= 0) {
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

    app_.PresentIfDirty();
  }
#ifdef DSLIBRIS_DEBUG
  app_.PrintStatus("APP exit: aptMainLoop returned false");
#endif
  return 0;
}
