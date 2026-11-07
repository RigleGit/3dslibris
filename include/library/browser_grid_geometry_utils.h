#pragma once

namespace browser_grid_geometry_utils {

bool RoundedRectContains(int x, int y, int w, int h, int radius);
// Returns the contiguous [left, right) span of the existing rounded mask.
void RoundedRectRowSpan(int y, int w, int h, int radius, int *left, int *right);
void FitRectPreserveAspect(int src_w, int src_h, int box_w, int box_h,
                           int *out_w, int *out_h);

} // namespace browser_grid_geometry_utils
