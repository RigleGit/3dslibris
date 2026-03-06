/*
    3dslibris - main.cpp

    An ebook reader for the Nintendo 3DS.
    Ported from dslibris (Nintendo DS).

    Copyright (C) 2007-2025 Ray Haleblian
    3DS port 2025

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

#include "app.h"
#include "book.h"
#include "button.h"
#include "expat.h"
#include "parse.h"
#include "text.h"
#include "version.h"

App *app;
char msg[256];

//! \param vblanks blanking intervals to wait, -1 for forever, default = -1
int halt(int vblanks) {
  // Flush once immediately so any preceding printf/console
  // output is visible on the very first frame.
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();

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
int halt(const char *msg, int vblanks) {
  printf("%s\n", msg);
  return halt(vblanks);
}

int main(int argc, char **argv) {
  // Initialize 3DS services
  gfxInitDefault();
  // Use console on bottom screen for init messages.
  // Once App::Run() starts, BlitToFramebuffer() takes over both screens.
  consoleInit(GFX_BOTTOM, NULL);

  printf("================================\n");
  printf("  3dslibris %s\n", VERSION);
  printf("================================\n\n");
  printf("Initializing...\n");

  // Flush the console banner so it's visible during initialization.
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();

  // Create book directory if it doesn't exist
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/3dslibris", 0777);
  mkdir("sdmc:/3ds/3dslibris/book", 0777);
  mkdir("sdmc:/3ds/3dslibris/font", 0777);
  mkdir("sdmc:/3ds/3dslibris/cache", 0777);
  mkdir("sdmc:/3ds/3dslibris/cache/covers", 0777);

  app = new App();
  int result = app->Run();

  delete app;

  // If Run() returned early (error), wait for user
  if (result != 0) {
    printf("\nPress START to exit.\n");
    // With double buffering, the console text lives in only one back buffer.
    // Flush+swap twice so both buffers contain the console output,
    // preventing garbled/flickering display.
    for (int i = 0; i < 2; i++) {
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
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

u8 GetParserFace(parsedata_t *pdata) {
  if (pdata->italic)
    return TEXT_STYLE_ITALIC;
  else if (pdata->bold)
    return TEXT_STYLE_BOLD;
  else
    return TEXT_STYLE_REGULAR;
}

void WriteBufferToCache(parsedata_t *pdata) {
  // Caching disabled for now
}

// Splash screen functions - stubbed out for 3DS
// The DS version used LZ77 decompression to VRAM
int getSize(u8 *source, u16 *dest, u32 arg) { return *(u32 *)source; }

u8 readByte(u8 *source) { return *source; }

void drawstack(u16 *screen) {
  // DS splash decompression not available on 3DS
  // TODO: implement 3DS splash screen
}
