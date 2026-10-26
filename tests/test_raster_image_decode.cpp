#include "formats/common/raster_image_decode.h"
#include "reader/image_view_utils.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <cstdio>

static void TestLandscapeImageZoomAndPan() {
  const auto one = image_view::Viewport(400,240,240,400,2,0,1);
  assert(one.left==0 && one.top==0 && one.width==1 && one.height==1);
  const auto half = image_view::Viewport(400,240,240,400,3,0.5f,0.5f);
  assert(std::fabs(half.width-2.0f/3)<0.00001f && half.height==1);
  const auto twice = image_view::Viewport(400,240,240,400,4,0.5f,0.5f);
  assert(std::fabs(twice.width-0.5f)<0.00001f && twice.height==1);
  const auto four = image_view::Viewport(400,240,240,400,6,0,1);
  assert(four.left==0 && std::fabs(four.width-0.25f)<0.00001f);
  assert(std::fabs(four.height-25.0f/36)<0.00001f);
  assert(std::fabs(four.top+four.height-1)<0.00001f);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  TestLandscapeImageZoomAndPan();
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
    const auto fit = image_view::Viewport(1000, 2000, 240, 400, 2, 0.5f, 0.5f);
    assert(fit.left == 0 && fit.top == 0 && fit.width == 1 && fit.height == 1);
    const auto zoom = image_view::Viewport(1000, 2000, 240, 400, 6, 0, 1);
    assert(zoom.left == 0 && zoom.top > 0.7f &&
           zoom.top + zoom.height <= 1.0001f);
    float dx, dy;
    image_view::MapPan(orientation, 1, 0, &dx, &dy);
    assert(orientation == 2 ? dx == 1 && dy == 0
                            : dx == 0 && dy == (orientation == 0 ? -1 : 1));
  }
  const auto required = image_view::RequiredSize(1000, 2000, 240, 400, 6);
  assert(required.width == 800 && required.height == 1600);
  puts("PASS: bounded image decode, transparency, retained view and viewport");
}
