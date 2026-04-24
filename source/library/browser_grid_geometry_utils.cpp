#include "library/browser_grid_geometry_utils.h"

namespace browser_grid_geometry_utils {

bool RoundedRectContains(int x, int y, int w, int h, int radius) {
  if (w <= 0 || h <= 0)
    return false;
  if (radius <= 0)
    return x >= 0 && y >= 0 && x < w && y < h;
  if (x < 0 || y < 0 || x >= w || y >= h)
    return false;

  const int r = radius;
  if ((x >= r && x < w - r) || (y >= r && y < h - r))
    return true;

  int cx = (x < r) ? r - 1 : w - r;
  int cy = (y < r) ? r - 1 : h - r;
  const int dx = x - cx;
  const int dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

void FitRectPreserveAspect(int src_w, int src_h, int box_w, int box_h,
                           int *out_w, int *out_h) {
  if (!out_w || !out_h)
    return;
  if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
    *out_w = 0;
    *out_h = 0;
    return;
  }

  const long long lhs = (long long)box_w * (long long)src_h;
  const long long rhs = (long long)box_h * (long long)src_w;
  if (lhs <= rhs) {
    *out_w = box_w;
    *out_h = (int)((long long)src_h * (long long)box_w / (long long)src_w);
  } else {
    *out_h = box_h;
    *out_w = (int)((long long)src_w * (long long)box_h / (long long)src_h);
  }

  if (*out_w < 1)
    *out_w = 1;
  if (*out_h < 1)
    *out_h = 1;
}

} // namespace browser_grid_geometry_utils
