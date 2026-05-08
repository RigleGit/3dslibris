/*
    3dslibris - color_utils.h
    Shared RGB565 color conversion utilities.
    Extracted from app.cpp, text.cpp, and ui_button_skin.cpp by Rigle.
*/

#ifndef COLOR_UTILS_H
#define COLOR_UTILS_H

#ifdef __3DS__
#include "3ds.h"
#else
#include <stdint.h>
typedef uint16_t u16;
#endif

// Convert floating-point R/G/B (0–255 range) to packed RGB565.
// Values are clamped to [0, 255].
static inline u16 RGB565FromU8(float r, float g, float b) {
  if (r < 0.0f)
    r = 0.0f;
  else if (r > 255.0f)
    r = 255.0f;
  if (g < 0.0f)
    g = 0.0f;
  else if (g > 255.0f)
    g = 255.0f;
  if (b < 0.0f)
    b = 0.0f;
  else if (b > 255.0f)
    b = 255.0f;
  return ((u16)(r / 8) << 11) | ((u16)(g / 4) << 5) | (u16)(b / 8);
}

static inline void UnpackRgb565(u16 pixel, int *r, int *g, int *b) {
  const int r5 = (pixel >> 11) & 0x1F;
  const int g6 = (pixel >> 5) & 0x3F;
  const int b5 = pixel & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

#endif // COLOR_UTILS_H
