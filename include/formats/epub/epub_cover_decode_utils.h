#pragma once

#include <algorithm>

namespace epub_cover_decode_utils {

inline bool ComputeCoverThumbSize(int img_w, int img_h, int max_w, int max_h,
                                  int *final_w, int *final_h,
                                  float *scale_out) {
  if (!final_w || !final_h || !scale_out || img_w <= 0 || img_h <= 0 ||
      max_w <= 0 || max_h <= 0) {
    return false;
  }
  const float scale_x = (float)img_w / (float)max_w;
  const float scale_y = (float)img_h / (float)max_h;
  const float scale = std::max(scale_x, scale_y);
  if (scale <= 0.0f)
    return false;
  int thumb_w = (int)(img_w / scale + 0.0001f);
  int thumb_h = (int)(img_h / scale + 0.0001f);
  if (thumb_w > max_w)
    thumb_w = max_w;
  if (thumb_h > max_h)
    thumb_h = max_h;
  thumb_w = std::max(1, thumb_w);
  thumb_h = std::max(1, thumb_h);
  *final_w = thumb_w;
  *final_h = thumb_h;
  *scale_out = scale;
  return true;
}

inline int ComputeJpegL2SubsampleFactor(int img_w, int img_h, int target_w,
                                        int target_h) {
  if (img_w <= 0 || img_h <= 0 || target_w <= 0 || target_h <= 0)
    return 0;
  int l2factor = 0;
  while ((img_w >> (l2factor + 1)) >= target_w + 2 &&
         (img_h >> (l2factor + 1)) >= target_h + 2 &&
         l2factor < 6) {
    l2factor++;
  }
  return l2factor;
}

} // namespace epub_cover_decode_utils
