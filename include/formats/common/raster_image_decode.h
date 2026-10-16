#pragma once

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <vector>
#include "formats/common/image_buffer.h"

namespace raster_image {
static const size_t kBitmapLimit = 4u * 1024u * 1024u;
static const size_t kDecoderLimit = 16u * 1024u * 1024u;

struct FreePixels {
  void operator()(uint16_t *p) const;
};
struct Bitmap {
  int width = 0, height = 0, original_width = 0, original_height = 0;
  size_t decoder_peak = 0;
  std::unique_ptr<uint16_t, FreePixels> pixels;
  bool Allocate(int w, int h);
  size_t PixelCount() const {
    return (size_t)width * height;
  }
};
// Inline pages retain straight alpha until composition onto the actual surface.
struct ArgbBitmap {
  int width=0,height=0,original_width=0,original_height=0;
  size_t decoder_peak=0;
  image_memory::Pixels pixels;
  bool Allocate(int w,int h) {
    if(w<=0 || h<=0 || (size_t)w>1500000u/(size_t)h ||
        !pixels.resize((size_t)w*h,6000000u)) return false;
    width=w;height=h;return true;
  }
};
bool DecodeArgb(const unsigned char*,size_t,int,int,ArgbBitmap*,const char**);
// Main-thread-only: decoder and bitmap budgets are not shared with workers.
// Failure leaves out unchanged. Target is a minimum detail requirement; no
// reduced-quality fallback is returned when either memory budget is exceeded.
bool Decode(const unsigned char *bytes,size_t length,int target_width,
            int target_height, uint16_t background, Bitmap *out,
            const char **error);
inline bool Decode(const std::vector<unsigned char> &bytes,int w,int h,
                   uint16_t bg,Bitmap *out,const char **error) {
  return Decode(bytes.data(),bytes.size(),w,h,bg,out,error);
}

// Same JPEG subsampling policy for CBZ and enlarged book images.
inline int SubsampleFactor(int width, int height, int target_width,
                           int target_height) {
  int factor = 0;
  while (factor < 6 && (width >> (factor + 1)) >= target_width + 2 &&
         (height >> (factor + 1)) >= target_height + 2)
    ++factor;
  return factor;
}
} // namespace raster_image
