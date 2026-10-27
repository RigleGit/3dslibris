#include "formats/common/fixed_layout_blit_utils.h"

#include "shared/color_utils.h"

#include <algorithm>
#include <cstring>

namespace fixed_layout_blit_utils {

static inline u16 InterpolateRgb565(u16 first, u16 second, int t) {
  if (first == second)
    return first;
  int r0, g0, b0, r1, g1, b1;
  UnpackRgb565(first, &r0, &g0, &b0);
  UnpackRgb565(second, &r1, &g1, &b1);
  const int r = r0 * (256 - t) + r1 * t + 128;
  const int g = g0 * (256 - t) + g1 * t + 128;
  const int b = b0 * (256 - t) + b1 * t + 128;
  return (u16)(((r >> 11) << 11) | ((g >> 10) << 5) | (b >> 11));
}

void BlitRgb565SurfaceScaledCrop(u16 *screen, int stride, int logical_width, int logical_height,
                                int x, int y, int draw_width,
                                int draw_height,
                                const u16 *pixels, size_t pixel_count,
                                int src_width, int src_height,
                                int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter) {
  if (!screen || !pixels || draw_width <= 0 || draw_height <= 0 ||
      src_width <= 0 || src_height <= 0 || crop_width <= 0 ||
      crop_height <= 0) {
    return;
  }

  const size_t required_pixels = (size_t)src_width * (size_t)src_height;
  if (pixel_count < required_pixels)
    return;

  crop_x = std::max(0, std::min(src_width - 1, crop_x));
  crop_y = std::max(0, std::min(src_height - 1, crop_y));
  crop_width = std::max(1, std::min(src_width - crop_x, crop_width));
  crop_height = std::max(1, std::min(src_height - crop_y, crop_height));

  if (crop_width == draw_width && crop_height == draw_height) {
    const int col0 = std::max(0, -x);
    const int col1 = std::min(draw_width, logical_width - x);
    if (col0 >= col1)
      return;
    const size_t copy_bytes = (size_t)(col1 - col0) * sizeof(u16);
    for (int row = 0; row < draw_height; row++) {
      const int dy = y + row;
      if (dy < 0 || dy >= logical_height)
        continue;
      const int src_y = crop_y + row;
      const int src_x = crop_x + col0;
      const int dx = x + col0;
      memcpy(&screen[(size_t)dy * (size_t)stride + (size_t)dx],
             &pixels[(size_t)src_y * (size_t)src_width + (size_t)src_x],
             copy_bytes);
    }
    return;
  }

  const int col0 = std::max(0, -x);
  const int col1 = std::min(draw_width, logical_width - x);
  const int row0 = std::max(0, -y);
  const int row1 = std::min(draw_height, logical_height - y);
  if (col0 >= col1 || row0 >= row1)
    return;

  struct ColumnSample {
    int x0, x1;
    int t;
  };
  // Only visible columns are retained, even when a zoomed page is very wide.
  std::vector<ColumnSample> columns((size_t)(col1 - col0));
  for (int col = col0; col < col1; col++) {
    ColumnSample &sample = columns[(size_t)(col - col0)];
    if (!high_quality_filter) {
      sample.x0 = crop_x + ((col * crop_width) / draw_width);
      continue;
    }
    const float src_xf = (float)crop_x +
        (((float)col + 0.5f) * (float)crop_width / (float)draw_width) - 0.5f;
    const float clamped_x = std::max((float)crop_x,
        std::min((float)(crop_x + crop_width - 1), src_xf));
    sample.x0 = (int)clamped_x;
    sample.x1 = std::min(crop_x + crop_width - 1, sample.x0 + 1);
    sample.t = (int)((clamped_x - (float)sample.x0) * 256.0f + 0.5f);
  }

  for (int row = row0; row < row1; row++) {
    const int dy = y + row;
    if (!high_quality_filter) {
      const int src_y = crop_y + ((row * crop_height) / draw_height);
      for (int col = col0; col < col1; col++) {
        screen[(size_t)dy * (size_t)stride + (size_t)(x + col)] =
            pixels[(size_t)src_y * (size_t)src_width +
                   (size_t)columns[(size_t)(col - col0)].x0];
      }
      continue;
    }
    const float src_yf = (float)crop_y +
        (((float)row + 0.5f) * (float)crop_height / (float)draw_height) - 0.5f;
    const float clamped_y = std::max((float)crop_y,
        std::min((float)(crop_y + crop_height - 1), src_yf));
    const int y0 = (int)clamped_y;
    const int y1 = std::min(crop_y + crop_height - 1, y0 + 1);
    const int ty = (int)((clamped_y - (float)y0) * 256.0f + 0.5f);
    for (int col = col0; col < col1; col++) {
      const int dx = x + col;
      const ColumnSample &sample = columns[(size_t)(col - col0)];
      const int x0 = sample.x0;
      const int x1 = sample.x1;
      const int tx = sample.t;
      const u16 p00 = pixels[(size_t)y0 * (size_t)src_width + (size_t)x0];
      const u16 p10 = pixels[(size_t)y0 * (size_t)src_width + (size_t)x1];
      // Identity rows need only horizontal interpolation. Rounded PDF bounds
      // often add one column to an otherwise native-size raster.
      if (ty == 0) {
        screen[(size_t)dy * (size_t)stride + (size_t)dx] =
            InterpolateRgb565(p00, p10, tx);
        continue;
      }
      const u16 p01 = pixels[(size_t)y1 * (size_t)src_width + (size_t)x0];
      const u16 p11 = pixels[(size_t)y1 * (size_t)src_width + (size_t)x1];
      if (p00 == p10 && p00 == p01 && p00 == p11) {
        screen[(size_t)dy * (size_t)stride + (size_t)dx] = p00;
        continue;
      }
      if (tx == 0) {
        screen[(size_t)dy * (size_t)stride + (size_t)dx] =
            InterpolateRgb565(p00, p01, ty);
        continue;
      }

      int r00 = 0, g00 = 0, b00 = 0;
      int r10 = 0, g10 = 0, b10 = 0;
      int r01 = 0, g01 = 0, b01 = 0;
      int r11 = 0, g11 = 0, b11 = 0;
      UnpackRgb565(p00, &r00, &g00, &b00);
      UnpackRgb565(p10, &r10, &g10, &b10);
      UnpackRgb565(p01, &r01, &g01, &b01);
      UnpackRgb565(p11, &r11, &g11, &b11);

      // Q8 axis weights sum to 65536 in two dimensions. Expanded RGB8
      // channels retain the previous colour range; every sum fits in 24 bits.
      const int w00 = (256 - tx) * (256 - ty);
      const int w10 = tx * (256 - ty);
      const int w01 = (256 - tx) * ty;
      const int w11 = tx * ty;
      const int r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 32768;
      const int g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 32768;
      const int b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 32768;
      screen[(size_t)dy * (size_t)stride + (size_t)dx] =
          (u16)(((r >> 19) << 11) | ((g >> 18) << 5) | (b >> 19));
    }
  }
}

} // namespace fixed_layout_blit_utils
