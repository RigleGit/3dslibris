#pragma once

#include "ui/text.h"

class Book {
public:
  Text *text;
  int background_draws;
  bool background_available;
  explicit Book(Text *value)
      : text(value), background_draws(0), background_available(true) {}
  bool DrawBottomGradientBackground() {
    if (!background_available) return false;
    ++background_draws;
    for (int y = 0; y < text->height; ++y)
      for (int x = 0; x < text->width; ++x)
        text->bottom[(size_t)y * text->stride + x] =
            (u16)(0x1000 + y * 7 + x * 13 + text->color_mode * 113);
    return true;
  }
};
