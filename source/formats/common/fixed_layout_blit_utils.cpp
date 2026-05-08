#include "formats/common/fixed_layout_blit_utils.h"

#include "shared/color_utils.h"
#include "ui/text.h"

#include <algorithm>

namespace fixed_layout_blit_utils {

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width,
                                int draw_height,
                                const std::vector<u16> &pixels,
                                int src_width, int src_height,
                                int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter) {
  if (!ts || !screen || pixels.empty() || draw_width <= 0 || draw_height <= 0 ||
      src_width <= 0 || src_height <= 0 || crop_width <= 0 ||
      crop_height <= 0) {
    return;
  }

  const size_t required_pixels = (size_t)src_width * (size_t)src_height;
  if (pixels.size() < required_pixels)
    return;

  crop_x = std::max(0, std::min(src_width - 1, crop_x));
  crop_y = std::max(0, std::min(src_height - 1, crop_y));
  crop_width = std::max(1, std::min(src_width - crop_x, crop_width));
  crop_height = std::max(1, std::min(src_height - crop_y, crop_height));

  const int stride = ts->display.height;
  const int logical_width = ts->display.width;
  ts->MarkScreenDirtyRect(screen, x, y, x + draw_width, y + draw_height);

  if (crop_width == draw_width && crop_height == draw_height) {
    for (int row = 0; row < draw_height; row++) {
      const int dy = y + row;
      if (dy < 0 || dy >= logical_height)
        continue;
      const int src_y = crop_y + row;
      for (int col = 0; col < draw_width; col++) {
        const int dx = x + col;
        if (dx < 0 || dx >= logical_width)
          continue;
        const int src_x = crop_x + col;
        screen[(size_t)dy * (size_t)stride + (size_t)dx] =
            pixels[(size_t)src_y * (size_t)src_width + (size_t)src_x];
      }
    }
    return;
  }

  for (int row = 0; row < draw_height; row++) {
    const int dy = y + row;
    if (dy < 0 || dy >= logical_height)
      continue;
    for (int col = 0; col < draw_width; col++) {
      const int dx = x + col;
      if (dx < 0 || dx >= logical_width)
        continue;

      if (!high_quality_filter) {
        const int src_x = crop_x +
                          ((col * crop_width) / std::max(1, draw_width));
        const int src_y = crop_y +
                          ((row * crop_height) / std::max(1, draw_height));
        screen[(size_t)dy * (size_t)stride + (size_t)dx] =
            pixels[(size_t)src_y * (size_t)src_width + (size_t)src_x];
        continue;
      }

      const float src_xf =
          (float)crop_x +
          (((float)col + 0.5f) * (float)crop_width / (float)draw_width) - 0.5f;
      const float src_yf =
          (float)crop_y +
          (((float)row + 0.5f) * (float)crop_height / (float)draw_height) - 0.5f;
      const float clamped_x =
          std::max((float)crop_x,
                   std::min((float)(crop_x + crop_width - 1), src_xf));
      const float clamped_y =
          std::max((float)crop_y,
                   std::min((float)(crop_y + crop_height - 1), src_yf));
      const int x0 = (int)clamped_x;
      const int y0 = (int)clamped_y;
      const int x1 = std::min(crop_x + crop_width - 1, x0 + 1);
      const int y1 = std::min(crop_y + crop_height - 1, y0 + 1);
      const float tx = clamped_x - (float)x0;
      const float ty = clamped_y - (float)y0;

      int r00 = 0, g00 = 0, b00 = 0;
      int r10 = 0, g10 = 0, b10 = 0;
      int r01 = 0, g01 = 0, b01 = 0;
      int r11 = 0, g11 = 0, b11 = 0;
      UnpackRgb565(pixels[(size_t)y0 * (size_t)src_width + (size_t)x0], &r00,
                   &g00, &b00);
      UnpackRgb565(pixels[(size_t)y0 * (size_t)src_width + (size_t)x1], &r10,
                   &g10, &b10);
      UnpackRgb565(pixels[(size_t)y1 * (size_t)src_width + (size_t)x0], &r01,
                   &g01, &b01);
      UnpackRgb565(pixels[(size_t)y1 * (size_t)src_width + (size_t)x1], &r11,
                   &g11, &b11);

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;
      screen[(size_t)dy * (size_t)stride + (size_t)dx] = RGB565FromU8(
          r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 0.5f,
          g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 0.5f,
          b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 0.5f);
    }
  }
}

} // namespace fixed_layout_blit_utils
