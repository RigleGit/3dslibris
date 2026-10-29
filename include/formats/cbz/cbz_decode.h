#pragma once

#include "formats/cbz/cbz_types.h"

#include <vector>
#include "formats/common/raster_image_source.h"

struct fz_context;

struct CbzDecodedPage {
  int original_width;
  int original_height;
  CbzBitmap source_bitmap;

  CbzDecodedPage()
      : original_width(0), original_height(0), source_bitmap() {}
};

const char *GetLastCbzDecodeError();
bool DecodeCbzPageImage(const std::vector<unsigned char> &bytes,
                        int max_zoom_index, int target_width,
                        int target_height, CbzDecodedPage *out);
// The reader owns one context; cover/worker callers retain the temporary API.
// Each decode clears its store before the borrowed entry bytes can expire.
bool DecodeCbzPageImageWithContext(const std::vector<unsigned char> &bytes,
                                  int max_zoom_index, int target_width,
                                  int target_height, CbzDecodedPage *out,
                                  fz_context **context,
                                  const raster_image::Rgb8Sink *sink = NULL);
void ResetCbzDecodeContext(fz_context **context);
bool ScaleCbzBitmap(const CbzBitmap &src, int dst_width, int dst_height,
                    bool high_quality, CbzBitmap *out);
