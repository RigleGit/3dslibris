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
#include "shared/fixed_layout_perf.h"
#include "shared/image_scale_utils.h"
#include "shared/stb_image_gif_utils.h"
#include "stb_image.h"

namespace cover_decode_utils {

bool DecodeImageToCoverThumb(Book *book, const unsigned char *data,
                             std::size_t size, int max_dimension) {
  if (!book || !data || size == 0 || size > (std::size_t)INT_MAX)
    return false;

  int img_w = 0;
  int img_h = 0;
  int channels = 0;
  const uint64_t header_start = fixed_perf::Now();
  if (!stbi_info_from_memory(data, (int)size, &img_w, &img_h, &channels) ||
      img_w <= 0 || img_h <= 0 ||
      (max_dimension > 0 && (img_w > max_dimension || img_h > max_dimension))) {
    fixed_perf::Record(book, -1, -1, "cover.header",
                       fixed_perf::Now() - header_start, 0, size, img_w, img_h);
    return false;
  }
  fixed_perf::Record(book, -1, -1, "cover.header",
                     fixed_perf::Now() - header_start, 1, size, img_w, img_h);
  fixed_perf::SampleMemory(book, -1, -1, "cover.before_decode");
  fixed_perf::Timer decode_timer(book, -1, -1, "cover.decode");
  unsigned char *pixels = stb_image_gif_utils::LoadFromMemory(
      data, (int)size, &img_w, &img_h, &channels, 3);
  decode_timer.End(pixels != nullptr, size, img_w, img_h);
  fixed_perf::SampleMemory(book, -1, -1, "cover.after_decode");
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
  u16 *new_pixels = new (std::nothrow) u16[(std::size_t)final_w * final_h];
  if (!new_pixels) {
    stbi_image_free(pixels);
    return false;
  }
  fixed_perf::SampleMemory(book, -1, -1, "cover.thumbnail_allocated");
  if (!image_scale_utils::ScaleRgbToRgb565Bilinear(
          pixels, img_w, img_h, final_w, final_h, new_pixels)) {
    delete[] new_pixels;
    stbi_image_free(pixels);
    return false;
  }

  stbi_image_free(pixels);

  delete[] book->coverPixels;
  book->coverPixels = new_pixels;
  book->coverWidth = final_w;
  book->coverHeight = final_h;
  fixed_perf::Record(book, -1, -1, "cover.thumbnail", 0, 1,
                     (std::size_t)final_w * final_h * sizeof(u16),
                     final_w, final_h);
  fixed_perf::SampleMemory(book, -1, -1, "cover.committed");
  return true;
}

} // namespace cover_decode_utils
