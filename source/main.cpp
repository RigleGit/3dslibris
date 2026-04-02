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

#include "main.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "version.h"

static void PresentCurrentFrameToBothBuffers(Text *presenter) {
  // Re-blit before each swap so both physical backbuffers receive the same
  // software-rendered frame. Swapping twice without re-blitting would simply
  // alternate between a fresh fatal screen and a stale previous frame.
  for (int i = 0; i < 2; i++) {
    if (presenter)
      presenter->BlitToFramebuffer();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
}

//! \param vblanks blanking intervals to wait, -1 for forever, default = -1
int halt(Text *presenter, int vblanks) {
  // Present the current frame to both buffers so we don't alternate between a
  // stale previous frame and the latest fatal/console screen.
  PresentCurrentFrameToBothBuffers(presenter);

  int timer = vblanks;
  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_START)
      break;
    if (timer == 0)
      break;
    else if (timer > 0)
      timer--;

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  return 1;
}

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
    APT_SetAppCpuTimeLimit(30);
  }

  printf("================================\n");
  printf("  3dslibris %s\n", VERSION);
  printf("================================\n\n");
  printf("Initializing...\n");

  // Flush the console banner so it's visible during initialization.
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();

  bool romfs_ready = (romfsInit() == 0);
  if (!romfs_ready)
    printf("[WARN] romfsInit failed; built-in CIA assets are unavailable.\n");

  // Create book directory if it doesn't exist
  mkdir("sdmc:/3ds", 0777);
  mkdir(paths::kSdmcBase, 0777);
  mkdir(paths::kBookDir, 0777);
  mkdir(paths::kFontDir, 0777);
  mkdir(paths::kResourceDir, 0777);
  mkdir(paths::kCacheBaseDir, 0777);
  mkdir(paths::kCoverCacheDir, 0777);

  App *app = new App();
  int result = app->Run();

  delete app;
  app = NULL;

  if (romfs_ready)
    romfsExit();

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

  gfxExit();
  return 0;
}
