/*
    3dslibris - main.cpp

    An ebook reader for the Nintendo 3DS.
    Ported from dslibris (Nintendo DS).

    Copyright (C) 2007-2025 Ray Haleblian
    3DS port 2026, modified by Rigle.

    Changes by Rigle (summary):
    - 3DS service/bootstrap flow and framebuffer lifecycle.
    - SD directory initialization for books/prefs/cache paths.
    - Main loop integration for App run/shutdown and 3DS cleanup.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "shared/main.h"

#include <3ds.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/app.h"
#include "shared/debug_log.h"
#include "shared/path_constants.h"
#include "app/version.h"

namespace {

#ifdef DSLIBRIS_DEBUG
void ShutdownTrace(const char *fmt, ...) {
  if (!fmt)
    return;

  FILE *log = fopen(paths::kLogFile, "a");
  if (!log)
    return;

  char timestamp[80];
  time_t rawtime;
  time(&rawtime);
  struct tm *info = localtime(&rawtime);
  if (!info) {
    fclose(log);
    return;
  }
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", info);

  fprintf(log, "[%s] ", timestamp);
  va_list args;
  va_start(args, fmt);
  vfprintf(log, fmt, args);
  va_end(args);
  fputc('\n', log);
  fclose(log);
}
#else
void ShutdownTrace(const char *fmt, ...) {
  (void)fmt;
}
#endif

} // namespace

static void PresentCurrentFrameToBothBuffers(Text *presenter) {
  // Re-blit before each swap so both physical backbuffers receive the same
  // software-rendered frame. Swapping twice without re-blitting would simply
  // alternate between a fresh fatal screen and a stale previous frame.
  for (int i = 0; i < 2; i++) {
    if (presenter)
      presenter->BlitToFramebuffer(); // Blit to backbuffer before each swap.
    gfxFlushBuffers();  // Ensure the backbuffer is updated before swapping.
    gfxSwapBuffers();   // Swap front and back buffers to present the frame.
    gspWaitForVBlank(); // Wait for vertical blank to avoid tearing and sync
                        // with display refresh.
  }
}

//! \param vblanks blanking intervals to wait, -1 for forever, default = -1
int halt(Text *presenter, int vblanks) {
  // Present the current frame to both buffers so we don't alternate between a
  // stale previous frame and the latest fatal/console screen.
  PresentCurrentFrameToBothBuffers(presenter);

  int timer = vblanks;
  while (aptMainLoop()) {
    hidScanInput(); // Updates the state of the input, must be called before
                    // reading keys.
    u32 kDown = hidKeysDown(); // Gets the keys that were just pressed in this
                               // frame (edge-triggered).
    if (kDown & KEY_START)
      break;
    if (timer == 0)
      break;
    else if (timer > 0)
      timer--;

    // In case of a long wait, keep the frame updated to show any status
    // messages.
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return 1;
}

// Overload for convenience when no presenter is available (e.g. early init
// failure).
//! \param vblanks blanking intervals to wait, -1 for forever, default = -1
int halt(Text *presenter, const char *msg, int vblanks) {
  printf("%s\n", msg);
  return halt(presenter, vblanks);
}

int main(int argc, char **argv) {
  // Initialize 3DS services
  gfxInitDefault();
  // Use console on bottom screen for init messages.
  // Once App::Run() starts, BlitToFramebuffer() takes over both screens.
  consoleInit(GFX_BOTTOM, NULL);

  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  if (is_new_3ds) {
    osSetSpeedupEnable(true);
    // APT_SetAppCpuTimeLimit(30); // Optional: increase CPU time limit for more
    // demanding tasks. Disabled to test.
  }

  printf("================================\n");
  printf("  3dslibris %s\n", VERSION);
  printf("================================\n\n");
  printf("Initializing...\n");

  // Flush the console banner so it's visible during initialization.
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();

  bool romfs_ready =
      (romfsInit() ==
       0); // RomFS is optional; if it fails, built-in assets won't be available
           // but the app can still run with SD card assets.
  if (!romfs_ready)
    printf("[WARN] romfsInit failed; built-in CIA assets are unavailable.\n");

  // Create book directory if it doesn't exist
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/config", 0777);
  mkdir(paths::GetSdmcBase().c_str(), 0777);
  mkdir(paths::GetBookDir().c_str(), 0777);
  mkdir(paths::GetFontDir().c_str(), 0777);
  mkdir(paths::GetResourceDir().c_str(), 0777);
  mkdir(paths::GetCacheBaseDir().c_str(), 0777);
  mkdir(paths::GetCoverCacheDir().c_str(), 0777);

  // Run the app, which takes over the main loop until exit.
  App *app = new App();
  App::SetInstance(
      app); // Set the global instance pointer for access in other modules.
  int result = app->Run();

  DBG_LOGF(app, "SHUTDOWN begin env=%s result=%d",
           app->IsHomebrewEnvironment() ? "3dsx/homebrew" : "cia/title",
           result);
  app->PrepareForShutdown();

  // Clean up and exit. App destructor will handle resource cleanup.
  DBG_LOG(app, "SHUTDOWN App destructor begin");
  App::SetInstance(nullptr);
  delete app;
  app = nullptr;
  ShutdownTrace("SHUTDOWN App destructor done");

  if (romfs_ready) {
    ShutdownTrace("SHUTDOWN romfsExit begin");
    romfsExit();
    ShutdownTrace("SHUTDOWN romfsExit done");
  }

  // If Run() returned early (error), wait for user
  if (result != 0) {
    printf("\nPress START to exit.\n");
    PresentCurrentFrameToBothBuffers(NULL);
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START)
        break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
  }

  ShutdownTrace("SHUTDOWN gfxExit begin");
  gfxExit(); // Cleanly exit the graphics system and return to the 3DS home
             // menu.
  ShutdownTrace("SHUTDOWN gfxExit done");
  ShutdownTrace("SHUTDOWN returning normally");
  return 0;
}
