#pragma once
#include "formats/common/pdf_view_utils.h"
#include "shared/orientation_utils.h"
#include <algorithm>
#include <cmath>

namespace image_view {
struct Size {
  int width, height;
};
inline Size RequiredSize(int width, int height, int screen_width,
                         int screen_height, int zoom) {
  if (width <= 0 || height <= 0 || screen_width <= 0 || screen_height <= 0)
    return {0, 0};
  const double fit =
      std::min((double)screen_width / width, (double)screen_height / height);
  const double scale = std::min(1.0, fit * pdf_view_utils::ZoomForIndex(zoom));
  return {std::max(1, (int)std::ceil(width * scale)),
          std::max(1, (int)std::ceil(height * scale))};
}
inline pdf_view_utils::NormalizedRect Viewport(int width, int height,
                                               int screen_width,
                                               int screen_height, int zoom,
                                               float x, float y) {
  return pdf_view_utils::ComputeViewportRect(
      width, height, pdf_view_utils::ZoomForIndex(zoom),
      screen_width, screen_height, x, y);
}
inline void MapPan(unsigned char orientation, float x, float y, float *dx,
                   float *dy) {
  if (orientation_utils::IsLandscape(orientation)) {
    *dx = x;
    *dy = y;
  } else if (orientation_utils::IsTurnedRight(orientation)) {
    *dx = -y;
    *dy = x;
  } else {
    *dx = y;
    *dy = -x;
  }
}
} // namespace image_view
