#include "formats/common/raster_image_decode.h"
#include "reader/image_view_utils.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cmath>
#include <algorithm>

static void TestResampling(const char *path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<unsigned char> png((std::istreambuf_iterator<char>(file)), {});
  for (int w = 1; w <= 7; ++w) for (int h = 1; h <= 6; ++h) {
    raster_image::ArgbBitmap image;
    const char *error = nullptr;
    assert(raster_image::DecodeArgb(png.data(), png.size(), w, h, &image, &error, true));
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      const double fx = std::max(0.0, (x + 0.5) * 4 / w - 0.5);
      const double fy = std::max(0.0, (y + 0.5) * 3 / h - 0.5);
      const int x0 = std::min(3, (int)fx), y0 = std::min(2, (int)fy);
      const double tx = fx - x0, ty = fy - y0;
      double channels[3] = {}, alpha = 0;
      for (int k = 0; k < 4; ++k) {
        const int sx = std::min(3, x0 + (k & 1));
        const int sy = std::min(2, y0 + (k >> 1));
        const double weight = ((k & 1) ? tx : 1-tx) * ((k & 2) ? ty : 1-ty);
        const int a = (sx*53+sy*71)%256;
        const int rgb[] = {(sx*63+sy*19)%256,(sx*11+sy*101)%256,(sx*79+sy*37)%256};
        alpha += weight * a;
        for (int c = 0; c < 3; ++c) channels[c] += weight * rgb[c] * a / 255.0;
      }
      const uint32_t pixel = image.pixels[(size_t)y*w+x];
      assert(std::abs((int)(pixel >> 24) - (int)(alpha+0.5)) <= 1);
      for (int c = 0; c < 3; ++c) {
        const int expected = alpha > 0 ? (int)(channels[c]*255/alpha+0.5) : 0;
        const int actual = (pixel >> ((2-c)*8)) & 255;
        assert(std::abs(actual-expected) <= 1);
      }
    }
  }
}

int main(int argc, char **argv) {
  assert(argc == 5);
  TestResampling(argv[4]);
  raster_image::Bitmap image;
  const char *error = NULL;
  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<unsigned char> png((std::istreambuf_iterator<char>(input)),
                                       {});
  assert(raster_image::Decode(png, 2, 1, 0xffff, &image, &error));
  assert(!error && image.width == 2 && image.height == 1);
  assert(image.pixels.get()[0] == 0x7bef); // half-transparent black over white
  assert(image.pixels.get()[1] == 0xffff); // transparent blue over white
  assert(image.decoder_peak > 0 &&
         image.decoder_peak <= raster_image::kDecoderLimit);
  uint16_t *valid = image.pixels.get();
  const std::vector<unsigned char> corrupt = {0, 1, 2, 3, 4};
  assert(!raster_image::Decode(corrupt, 2, 1, 0, &image, &error));
  assert(error && image.pixels.get() == valid &&
         image.pixels.get()[0] == 0x7bef);
  assert(!raster_image::Decode(png, 0, 1, 0, &image, &error));
  std::ifstream huge_file(argv[2], std::ios::binary);
  const std::vector<unsigned char> huge(
      (std::istreambuf_iterator<char>(huge_file)), {});
  assert(!raster_image::Decode(huge, 4000, 4000, 0, &image, &error));
  assert(image.pixels.get() == valid);
  // Enough pixels for a tiny target, but PNG's full-resolution decode must
  // obey the decoder allocation cap. A compressed blank 3000x3000 image.
  assert(!raster_image::Decode(huge, 2, 2, 0, &image, &error));
  assert(image.pixels.get() == valid);
  std::ifstream gif_file(argv[3], std::ios::binary);
  const std::vector<unsigned char> gif(
      (std::istreambuf_iterator<char>(gif_file)), {});
  assert(raster_image::Decode(gif, 1, 1, 0xffff, &image, &error));
  assert(image.width == 1 && image.height == 1);
  assert(image.pixels.get()[0] ==
         0xf800); // first (red), not final (blue) GIF frame
  image=raster_image::Bitmap();
  raster_image::Bitmap full, extra, blocked;
  assert(full.Allocate(2048, 1024));
  assert(extra.Allocate(2048,1024)); // Exactly 8 MiB is allowed.
  assert(!blocked.Allocate(1,1));
  extra=raster_image::Bitmap();
  assert(blocked.Allocate(1,1)); // Releasing returns budget.
  assert(!extra.Allocate(0, 1) && !extra.Allocate(2147483647, 2147483647));
  for (unsigned char orientation : {0, 1, 2}) {
    float dx, dy;
    image_view::MapPan(orientation, 1, 0, &dx, &dy);
    assert(orientation == 2 ? dx == 1 && dy == 0
                            : dx == 0 && dy == (orientation == 0 ? -1 : 1));
  }
  const auto required = image_view::RequiredSize(1000, 2000, 240, 400, 6);
  assert(required.width == 800 && required.height == 1600);
  puts("PASS: bounded image decode, transparency, retained view and viewport");
}
