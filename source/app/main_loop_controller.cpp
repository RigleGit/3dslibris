#include "app/main_loop_controller.h"

#include <3ds.h>

#include "app/app.h"
#include "library/browser_warmup_utils.h"

MainLoopController::MainLoopController(App &app) : app_(app) {}

int MainLoopController::RunMainLoop() {
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();

    if (app_.GetMode() != AppMode::Book && app_.GetMode() != AppMode::Opening) {
      bool allow_jobs = true;
      if (app_.GetMode() == AppMode::Browser) {
        allow_jobs = browser_warmup_utils::IsBrowserWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
      }
      if (allow_jobs)
        app_.ProcessJobs(3);
    }

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
      if (app_.IsBrowserDirty())
        app_.browser_draw();
      break;

    case AppMode::Quit:
      app_.PersistPrefs();
      return 0;

    case AppMode::Prefs:
      app_.PrefsHandleEvent();
      if (app_.IsPrefsDirty())
        app_.PrefsDraw();
      break;

    case AppMode::PrefsFont:
    case AppMode::PrefsFontBold:
    case AppMode::PrefsFontItalic:
    case AppMode::PrefsFontBoldItalic:
      app_.RunFontMenuFrame();
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
  return 0;
}
