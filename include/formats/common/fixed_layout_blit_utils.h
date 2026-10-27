#pragma once

#ifdef __3DS__
#include <3ds.h>
#else
#include <stdint.h>
typedef uint16_t u16;
#endif

#include <vector>
#include <stddef.h>

class Text;

namespace fixed_layout_blit_utils {

// Scale into a caller-owned RGB565 surface without Text or screen state.
void BlitRgb565SurfaceScaledCrop(
    u16 *surface, int stride, int logical_width, int logical_height,
    int x, int y, int draw_width, int draw_height,
    const u16 *pixels, size_t pixel_count, int src_width, int src_height,
    int crop_x, int crop_y, int crop_width, int crop_height,
    bool high_quality_filter,
    int clip_x0 = 0, int clip_y0 = 0,
    int clip_x1 = 0x7fffffff, int clip_y1 = 0x7fffffff);

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width,
                                int draw_height,
                                const u16 *pixels, size_t pixel_count,
                                int src_width, int src_height,
                                int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter);

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width,
                                int draw_height,
                                const std::vector<u16> &pixels,
                                int src_width, int src_height,
                                int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter);

} // namespace fixed_layout_blit_utils
