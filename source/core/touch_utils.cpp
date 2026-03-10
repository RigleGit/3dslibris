/*
    3dslibris - touch_utils.cpp
    New helper module for Nintendo 3DS port by Rigle.

    Summary:
    - Normalizes multiple touch coordinate candidates (mapped/raw transforms).
    - Centralizes robust hit-testing with optional slack for small buttons.
    - Provides shared helpers for footer-band and bounds-aware touch handling.
*/

#include "touch_utils.h"

#include <3ds.h>

#include "app.h"

namespace {

bool EnclosesWithSlack(Button *button, int x, int y, int slack) {
  if (!button)
    return false;
  if (slack <= 0)
    return button->EnclosesPoint((u16)x, (u16)y);
  for (int dy = -slack; dy <= slack; dy += slack) {
    for (int dx = -slack; dx <= slack; dx += slack) {
      int sx = x + dx;
      int sy = y + dy;
      if (sx < 0 || sy < 0 || sx > 239 || sy > 319)
        continue;
      if (button->EnclosesPoint((u16)sx, (u16)sy))
        return true;
    }
  }
  return false;
}

} // namespace

namespace touch {

void BuildCandidates(App *app, TouchCandidates *out) {
  if (!out)
    return;
  for (int i = 0; i < TouchCandidates::kCount; i++) {
    out->points[i].x = -1;
    out->points[i].y = -1;
  }
  if (!app)
    return;

  touchPosition mapped = app->TouchRead();

  // Single source of truth: use the app-level mapped coordinate only.
  // Multi-transform candidates can hit mirrored controls in turned modes.
  out->points[0] = {(int)mapped.px, (int)mapped.py};
}

bool InScreenBounds(int x, int y) {
  return x >= 0 && y >= 0 && x <= 239 && y <= 319;
}

bool HitsButton(const TouchCandidates &candidates, Button *button, int slack) {
  for (int i = 0; i < TouchCandidates::kCount; i++) {
    int x = candidates.points[i].x;
    int y = candidates.points[i].y;
    if (!InScreenBounds(x, y))
      continue;
    if (EnclosesWithSlack(button, x, y, slack))
      return true;
  }
  return false;
}

bool FirstXInBottomBand(const TouchCandidates &candidates, int y_min,
                        int *x_out) {
  if (!x_out)
    return false;
  for (int i = 0; i < TouchCandidates::kCount; i++) {
    int x = candidates.points[i].x;
    int y = candidates.points[i].y;
    if (!InScreenBounds(x, y))
      continue;
    if (y >= y_min) {
      *x_out = x;
      return true;
    }
  }
  return false;
}

} // namespace touch
