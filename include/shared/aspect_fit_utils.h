#pragma once

#include <algorithm>
#include <stdint.h>

namespace aspect_fit_utils {

struct Placement {
  int x;
  int y;
  int width;
  int height;
};

inline int ClampPositive(int value, int fallback) {
  return value > 0 ? value : fallback;
}

inline Placement FitInsideBox(int box_x, int box_y,
                              int box_width, int box_height,
                              int src_width, int src_height,
                              bool allow_upscale) {
  Placement out = {box_x, box_y, 1, 1};

  box_width = ClampPositive(box_width, 1);
  box_height = ClampPositive(box_height, 1);
  src_width = ClampPositive(src_width, 1);
  src_height = ClampPositive(src_height, 1);

  // No-upscale mode is used by extracted browser cover thumbnails. It keeps
  // the old cover-compatible truncation behavior: never enlarge, preserve the
  // full image, and floor the limited axis instead of PAGE-style rounding.
  if (!allow_upscale && src_width <= box_width && src_height <= box_height) {
    out.width = src_width;
    out.height = src_height;
    out.x = box_x + std::max(0, (box_width - src_width) / 2);
    out.y = box_y + std::max(0, (box_height - src_height) / 2);
    return out;
  }

  if (!allow_upscale) {
    const int64_t width_limited =
        (int64_t)box_width * (int64_t)src_height;
    const int64_t height_limited =
        (int64_t)box_height * (int64_t)src_width;
    int draw_width = box_width;
    int draw_height = box_height;
    if (width_limited >= height_limited) {
      draw_width =
          (int)std::max<int64_t>(1, ((int64_t)src_width * box_height) /
                                      src_height);
    } else {
      draw_height =
          (int)std::max<int64_t>(1, ((int64_t)src_height * box_width) /
                                      src_width);
    }
    draw_width = std::min(draw_width, src_width);
    draw_height = std::min(draw_height, src_height);
    out.width = draw_width;
    out.height = draw_height;
    out.x = box_x + std::max(0, (box_width - draw_width) / 2);
    out.y = box_y + std::max(0, (box_height - draw_height) / 2);
    return out;
  }

  // Upscale mode is used by inline PAGE image placement. It preserves the old
  // PAGE-compatible fixed-point scale and +512 rounding, so small PAGE images
  // may grow to fill the available box.
  int64_t scale_x = ((int64_t)box_width * 1024) / src_width;
  int64_t scale_y = ((int64_t)box_height * 1024) / src_height;
  int64_t scale = std::min(scale_x, scale_y);
  scale = std::max<int64_t>(1, scale);

  int draw_width =
      (int)std::max<int64_t>(1, ((int64_t)src_width * scale + 512) / 1024);
  int draw_height =
      (int)std::max<int64_t>(1, ((int64_t)src_height * scale + 512) / 1024);

  out.width = draw_width;
  out.height = draw_height;
  out.x = box_x + std::max(0, (box_width - draw_width) / 2);
  out.y = box_y + std::max(0, (box_height - draw_height) / 2);
  return out;
}

} // namespace aspect_fit_utils
