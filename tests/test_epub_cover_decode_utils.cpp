#include "formats/epub/epub_cover_decode_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectSizeEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

} // namespace

int main() {
  int thumb_w = 0;
  int thumb_h = 0;
  float scale = 0.0f;
  ExpectTrue("computes tall cover thumbnail",
             epub_cover_decode_utils::ComputeCoverThumbSize(
                 1600, 2400, 85, 115, &thumb_w, &thumb_h, &scale));
  ExpectEq("tall cover width", thumb_w, 76);
  ExpectEq("tall cover height", thumb_h, 115);

  ExpectEq("large jpeg uses 1/16 decode",
           epub_cover_decode_utils::ComputeJpegL2SubsampleFactor(
               1600, 2400, thumb_w, thumb_h),
           4);
  ExpectEq("medium jpeg uses 1/4 decode",
           epub_cover_decode_utils::ComputeJpegL2SubsampleFactor(
               512, 768, 76, 115),
           2);
  ExpectEq("small jpeg stays full resolution",
           epub_cover_decode_utils::ComputeJpegL2SubsampleFactor(
               100, 135, 76, 103),
           0);

  size_t decoded_bytes = 0;
  ExpectTrue("estimate rgb bytes for 4k square",
             epub_cover_decode_utils::EstimateDecodedRgbBytes(
                 4096, 4096, 3, &decoded_bytes));
  ExpectSizeEq("4k square rgb bytes", decoded_bytes, 4096u * 4096u * 3u);

  ExpectFalse("reject invalid component count",
              epub_cover_decode_utils::EstimateDecodedRgbBytes(
                  320, 240, 0, &decoded_bytes));
  ExpectFalse("reject non-positive width",
              epub_cover_decode_utils::EstimateDecodedRgbBytes(
                  0, 240, 3, &decoded_bytes));
  return 0;
}
