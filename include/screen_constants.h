/*
    3dslibris - screen_constants.h
    Screen dimensions, margins, and splash positioning constants.
    Extracted from main.h by Rigle.
*/

#pragma once

// Margins
#define MARGINLEFT 12
#define MARGINRIGHT 12
#define MARGINTOP 10
#define MARGINBOTTOM 36
#define LINESPACING 0

// 3DS screen dimensions in landscape mode:
//   Bottom screen: 320 x 240 physical pixels
//   Top screen:    400 x 240 physical pixels
//
// Software buffer uses: screen[sy * PAGE_HEIGHT + sx]
//   sx = horizontal (pen.x), 0 to PAGE_HEIGHT-1
//   sy = vertical   (pen.y), 0 to PAGE_WIDTH-1
// Buffer allocated as PAGE_HEIGHT * PAGE_HEIGHT (square, like DS VRAM).
#define PAGE_HEIGHT 400
#define PAGE_WIDTH 240

// Splash screen positioning
#define SPLASH_LEFT 28
#define SPLASH_TOP 44
