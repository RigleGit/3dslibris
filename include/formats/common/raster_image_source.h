#pragma once
#include <cstddef>
#include <cstdint>
#include <climits>

namespace raster_image {
// Borrowed decoded samples, valid only during the synchronous consume call.
// Matches fixed-layout conversion: gray[+alpha] or RGB[+alpha], with any alpha
// already premultiplied/composed by the decoder. The trailing component is ignored.
struct Rgb8View {
  const unsigned char *samples;
  size_t byte_count;
  int width, height, stride, components;
  bool Valid() const {
    return samples && width > 0 && height > 0 && components >= 1 &&
           components <= 4 && width <= INT_MAX / components &&
           stride >= width * components &&
           byte_count >= (uint64_t)(height - 1) * stride + width * components;
  }
  uint16_t Pixel(int x, int y) const {
    const unsigned char *p = samples + (size_t)y * stride + x * components;
    const unsigned r = p[0], g = components < 3 ? r : p[1],
                   b = components < 3 ? r : p[2];
    return (uint16_t)((r >> 3 << 11) | (g >> 2 << 5) | (b >> 3));
  }
};
// Optional consumer for main-thread fixed-layout decoding. False requests the
// normal complete CPU bitmap while the original pixmap is still available.
struct Rgb8Sink {
  void *user;
  bool (*consume)(void *, const Rgb8View &, float logical_width, float logical_height);
};
} // namespace raster_image
