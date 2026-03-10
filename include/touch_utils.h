/*
    3dslibris - touch_utils.h
    New helper module for Nintendo 3DS port by Rigle.

    Summary:
    - Touch candidate normalization API shared across menus.
    - Encapsulates robust button hit testing and bottom-band helpers.
*/

#pragma once

#include "button.h"

class App;

struct TouchPoint {
  int x;
  int y;
};

struct TouchCandidates {
  static const int kCount = 4;
  TouchPoint points[kCount];
};

namespace touch {

void BuildCandidates(App *app, TouchCandidates *out);
bool InScreenBounds(int x, int y);
bool HitsButton(const TouchCandidates &candidates, Button *button, int slack);
bool FirstXInBottomBand(const TouchCandidates &candidates, int y_min, int *x_out);

} // namespace touch
