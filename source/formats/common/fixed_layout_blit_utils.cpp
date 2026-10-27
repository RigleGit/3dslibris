#include "formats/common/fixed_layout_blit_utils.h"

#include "shared/orientation_utils.h"
#include "ui/text.h"

#include <algorithm>

namespace fixed_layout_blit_utils {

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width, int draw_height,
                                const u16 *pixels, size_t pixel_count,
                                int src_width, int src_height,
                                int crop_x, int crop_y, int crop_width, int crop_height,
                                bool high_quality_filter) {
  if (!ts || !screen || !pixels || src_width <= 0 || src_height <= 0 ||
      draw_width <= 0 || draw_height <= 0 || crop_width <= 0 || crop_height <= 0 ||
      pixel_count < (size_t)src_width * (size_t)src_height)
    return;
  const int logical_width = ts->LogicalWidthFor(screen == ts->screenleft);
  if (screen == ts->screenleft && ts->IsWideTopScreenActive()) {
    const bool landscape = orientation_utils::IsLandscape(ts->GetOrientation());
    const int dx = landscape ? 2 : 1, dy = landscape ? 1 : 2;
    u16 *wide = ts->BeginWideTopBlit(x, y, draw_width, draw_height);
    if (wide) {
      const int wide_stride = logical_width * dx;
      BlitRgb565SurfaceScaledCrop(wide, wide_stride, wide_stride,
                  logical_height * dy, x * dx, y * dy,
                  draw_width * dx, draw_height * dy, pixels, pixel_count,
                  src_width, src_height, crop_x, crop_y, crop_width, crop_height,
                  high_quality_filter);
      // Keep the logical mirror for alpha-blended UI and screen snapshots.
      // Average each native pair instead of running a second source scaler.
      const int x0 = std::max(0, x);
      const int x1 = std::min(logical_width, x + draw_width);
      const int y0 = std::max(0, y);
      const int y1 = std::min(logical_height, y + draw_height);
      const int stride = ts->BufferStride();
      const int pair_offset = dx == 2 ? 1 : wide_stride;
      for (int sy = y0; sy < y1; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          const u16 *pair = wide + (size_t)(sy * dy) * wide_stride + sx * dx;
          // Halve each RGB565 channel separately, without cross-channel carry.
          const u16 pixel = (pair[0] & pair[pair_offset]) +
              (((pair[0] ^ pair[pair_offset]) & 0xF7DEu) >> 1);
          screen[(size_t)sy * stride + sx] = pixel;
        }
      }
      return;
    }
  }
  ts->MarkScreenDirtyRect(screen, x, y, x + draw_width, y + draw_height);
  BlitRgb565SurfaceScaledCrop(screen, ts->BufferStride(), logical_width, logical_height,
              x, y, draw_width, draw_height, pixels, pixel_count,
              src_width, src_height, crop_x, crop_y, crop_width, crop_height,
              high_quality_filter);
}

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width, int draw_height,
                                const std::vector<u16> &pixels,
                                int src_width, int src_height,
                                int crop_x, int crop_y, int crop_width, int crop_height,
                                bool high_quality_filter) {
  BlitRgb565BitmapScaledCrop(ts, screen, logical_height, x, y, draw_width, draw_height,
      pixels.data(), pixels.size(), src_width, src_height, crop_x, crop_y,
      crop_width, crop_height, high_quality_filter);
}

} // namespace fixed_layout_blit_utils
