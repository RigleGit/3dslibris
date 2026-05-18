/*
    3dslibris - cover_decode_utils.cpp
    Shared cover-thumbnail decoder for raster cover bytes (JPEG/PNG/GIF/BMP).
    Replaces the previous DecodeAndScaleToCover copies in mobi_cover_extract,
    fb2, and similar format files.
*/

#include "shared/cover_decode_utils.h"

#include <algorithm>
#include <climits>

#include "book/book.h"
#include "book/cover_layout_constants.h"
#include "shared/aspect_fit_utils.h"
#include "stb_image.h"

namespace cover_decode_utils {

bool DecodeImageToCoverThumb(Book *book, const unsigned char *data,
                             std::size_t size, int max_dimension) {
  if (!book || !data || size == 0 || size > (std::size_t)INT_MAX)
    return false;

  int img_w = 0;
  int img_h = 0;
  int channels = 0;
  unsigned char *pixels = stbi_load_from_memory(data, (int)size, &img_w, &img_h,
                                                &channels, 3);
  if (!pixels)
    return false;

  if (img_w <= 0 || img_h <= 0 ||
      (max_dimension > 0 && (img_w > max_dimension || img_h > max_dimension))) {
    stbi_image_free(pixels);
    return false;
  }

  const aspect_fit_utils::Placement placement =
      aspect_fit_utils::FitInsideBox(
          0, 0, cover_layout::kBrowserCoverThumbWidth,
          cover_layout::kBrowserCoverThumbHeight, img_w, img_h, false);
  const int final_w = placement.width;
  const int final_h = placement.height;
  if (final_w <= 0 || final_h <= 0) {
    stbi_image_free(pixels);
    return false;
  }
  const float scale =
      std::max((float)img_w / (float)final_w,
               (float)img_h / (float)final_h);

  u16 *new_pixels = new (std::nothrow) u16[(std::size_t)final_w * (std::size_t)final_h];
  if (!new_pixels) {
    stbi_image_free(pixels);
    return false;
  }

  for (int y = 0; y < final_h; y++) {
    int src_y = (int)(y * scale);
    if (src_y >= img_h)
      src_y = img_h - 1;
    const unsigned char *row = pixels + (std::size_t)src_y * (std::size_t)img_w * 3u;
    for (int x = 0; x < final_w; x++) {
      int src_x = (int)(x * scale);
      if (src_x >= img_w)
        src_x = img_w - 1;
      const unsigned char *px = row + (std::size_t)src_x * 3u;
      const u16 r = (px[0] >> 3) & 0x1F;
      const u16 g = (px[1] >> 2) & 0x3F;
      const u16 b = (px[2] >> 3) & 0x1F;
      new_pixels[y * final_w + x] = (u16)((r << 11) | (g << 5) | b);
    }
  }

  stbi_image_free(pixels);

  delete[] book->coverPixels;
  book->coverPixels = new_pixels;
  book->coverWidth = final_w;
  book->coverHeight = final_h;
  return true;
}

} // namespace cover_decode_utils
