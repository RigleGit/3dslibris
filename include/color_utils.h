/*
    3dslibris - color_utils.h
    Shared RGB565 color conversion utilities.
    Extracted from app.cpp, text.cpp, and ui_button_skin.cpp by Rigle.
*/

#ifndef COLOR_UTILS_H
#define COLOR_UTILS_H

#include "3ds.h" // for u16

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

#endif // COLOR_UTILS_H
