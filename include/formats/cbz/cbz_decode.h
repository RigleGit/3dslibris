#pragma once

#include "formats/cbz/cbz_types.h"

#include <vector>

struct CbzDecodedPage {
  int original_width;
  int original_height;
  CbzBitmap source_bitmap;

  CbzDecodedPage()
      : original_width(0), original_height(0), source_bitmap() {}
};

const char *GetLastCbzDecodeError();
bool DecodeCbzPageImage(const std::vector<unsigned char> &bytes,
                        int max_zoom_index, CbzDecodedPage *out);
bool ScaleCbzBitmap(const CbzBitmap &src, int dst_width, int dst_height,
                    bool high_quality, CbzBitmap *out);
