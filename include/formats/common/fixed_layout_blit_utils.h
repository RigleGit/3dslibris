#pragma once

#ifdef __3DS__
#include <3ds.h>
#else
#include <stdint.h>
typedef uint16_t u16;
#endif

#include <vector>

class Text;

namespace fixed_layout_blit_utils {

void BlitRgb565BitmapScaledCrop(Text *ts, u16 *screen, int logical_height,
                                int x, int y, int draw_width,
                                int draw_height,
                                const std::vector<u16> &pixels,
                                int src_width, int src_height,
                                int crop_x, int crop_y,
                                int crop_width, int crop_height,
                                bool high_quality_filter);

} // namespace fixed_layout_blit_utils
