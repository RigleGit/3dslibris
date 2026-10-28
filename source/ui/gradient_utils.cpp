#include "ui/gradient_utils.h"

#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

#include "shared/color_utils.h"
#include "ui/text.h"
#include "ui/theme_colors.h"

namespace gradient_utils {

void DrawToScreen(Text *ts, int color_mode, u16 *target_screen, int logical_h)
{
  if (!ts || !target_screen)
    return;
  const int w = ts->LogicalWidthFor(target_screen == ts->screenleft);
  const int stride = ts->BufferStride();
  if (w <= 0 || stride <= 0 || logical_h <= 0)
    return;

  struct GradientCache {
    std::vector<u16> pixels;
    int width = 0, height = 0, color_mode = -1;
  };
  // The 400px and 320px screens alternate on every page. Retain one bounded
  // slot for each, replacing its contents on theme or orientation changes.
  static GradientCache caches[2];
  GradientCache &cache = caches[std::max(w, logical_h) <= 320 ? 0 : 1];
  std::vector<u16> &gradient = cache.pixels;

  const ThemePalette &palette = GetThemePalette(color_mode);

  if (gradient.empty() || cache.width != w || cache.height != logical_h ||
      cache.color_mode != color_mode)
  {
    gradient.resize((size_t)w * (size_t)logical_h);
    cache.width = w;
    cache.height = logical_h;
    cache.color_mode = color_mode;
    std::vector<float> column_vignette(w);
    for (int x = 0; x < w; ++x) {
      const float dx = w > 1
          ? ((float)x - (float)(w - 1) * 0.5f) / ((float)(w - 1) * 0.5f)
          : 0.0f;
      column_vignette[x] = 1.0f - 0.12f * powf(fabsf(dx), 1.8f);
    }
    static const u8 kBayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };

    for (int y = 0; y < logical_h; y++)
    {
      const float tY =
          (logical_h > 1) ? ((float)y / (float)(logical_h - 1)) : 0.0f;
      for (int x = 0; x < w; x++)
      {
        float r = palette.bgTopR + (palette.bgBotR - palette.bgTopR) * tY;
        float g = palette.bgTopG + (palette.bgBotG - palette.bgTopG) * tY;
        float b = palette.bgTopB + (palette.bgBotB - palette.bgTopB) * tY;

        const float vignette = column_vignette[x];

        const float bayer =
            (((float)kBayer4x4[y & 3][x & 3] + 0.5f) / 16.0f) - 0.5f;

        const u32 h0 = (u32)x * 73856093u;
        const u32 h1 = (u32)y * 19349663u;
        const u32 h2 = (h0 ^ h1 ^ 0x9E3779B9u);
        const float noise =
            ((((h2 >> 8) & 0xFF) / 255.0f) - 0.5f) * 0.6f;

        r = r * vignette + bayer * 3.8f + noise;
        g = g * vignette + bayer * 1.9f + noise * 0.6f;
        b = b * vignette + bayer * 3.8f + noise;

        gradient[(size_t)y * (size_t)w + (size_t)x] = RGB565FromU8(r, g, b);
      }
    }
  }

  for (int y = 0; y < logical_h; y++)
  {
    u16 *dst = target_screen + (size_t)y * (size_t)stride;
    const u16 *src = gradient.data() + (size_t)y * (size_t)w;
    memcpy(dst, src, (size_t)w * sizeof(u16));
  }
}

} // namespace gradient_utils
