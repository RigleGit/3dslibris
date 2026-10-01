#include "formats/cbz/cbz_decode.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

std::vector<unsigned char> ReadFile(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    Fail(std::string("unable to open file: ") + path);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectEmptyDecoded(const CbzDecodedPage &page) {
  ExpectTrue("failed decode clears dimensions", page.original_width == 0 &&
             page.original_height == 0 && page.source_bitmap.width == 0 &&
             page.source_bitmap.height == 0);
  ExpectTrue("failed decode clears pixels", page.source_bitmap.pixels.empty());
  ExpectTrue("decode failure supplies diagnostic", GetLastCbzDecodeError()[0] != '\0');
}

void TestPngPixelsAndFailureRecovery(const char *path) {
  const std::vector<unsigned char> bytes = ReadFile(path);
  CbzDecodedPage decoded;
  ExpectTrue("decode PNG", DecodeCbzPageImage(bytes, 5, 240, 400, &decoded));
  ExpectTrue("PNG original dimensions", decoded.original_width == 8 && decoded.original_height == 8);
  ExpectTrue("PNG bitmap dimensions", decoded.source_bitmap.width == 8 && decoded.source_bitmap.height == 8);
  ExpectTrue("PNG bitmap size", decoded.source_bitmap.pixels.size() == 64);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
    // Fixture RGB = (32*x, 32*y, 160); literal RGB565 contract, no renderer oracle.
    const uint16_t expected = (uint16_t)((x * 4 << 11) | (y * 8 << 5) | 20);
    ExpectTrue("PNG exact RGB565 pixel and row order", decoded.source_bitmap.pixels[y * 8 + x] == expected);
  }
  const std::vector<unsigned char> invalids[] = {
      std::vector<unsigned char>(), std::vector<unsigned char>(12, 'x'),
      std::vector<unsigned char>(bytes.begin(), bytes.begin() + 33)};
  for (const auto &invalid : invalids) {
    ExpectTrue("reject malformed image", !DecodeCbzPageImage(invalid, 5, 240, 400, &decoded));
    ExpectEmptyDecoded(decoded);
    ExpectTrue("decode valid after failure", DecodeCbzPageImage(bytes, 5, 240, 400, &decoded));
    ExpectTrue("successful decode clears error", GetLastCbzDecodeError()[0] == '\0');
    ExpectTrue("recovered decoded pixels", decoded.source_bitmap.pixels.size() == 64 &&
               decoded.source_bitmap.pixels.front() == 20 &&
               decoded.source_bitmap.pixels.back() == 0xE714);
  }
  ExpectTrue("decode to smaller fit target", DecodeCbzPageImage(bytes, 2, 4, 4, &decoded));
  ExpectTrue("fit target dimensions", decoded.source_bitmap.width == 4 && decoded.source_bitmap.height == 4);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
    ExpectTrue("downsample averages gradient centers", decoded.source_bitmap.pixels[y * 4 + x] ==
               (uint16_t)(((x * 8 + 2) << 11) | ((y * 16 + 4) << 5) | 20));
}

void TestJpegDecode(const char *path) {
  CbzDecodedPage decoded;
  ExpectTrue("decode JPEG", DecodeCbzPageImage(ReadFile(path), 5, 240, 400, &decoded));
  ExpectTrue("JPEG original dimensions", decoded.original_width == 8 && decoded.original_height == 8);
  ExpectTrue("JPEG bitmap dimensions", decoded.source_bitmap.width == 8 && decoded.source_bitmap.height == 8);
  ExpectTrue("JPEG bitmap size", decoded.source_bitmap.pixels.size() == 64);
  // Lossy JPEG may change individual samples, but must retain increasing R/G.
  ExpectTrue("JPEG horizontal red gradient", (decoded.source_bitmap.pixels[7] >> 11) >
             (decoded.source_bitmap.pixels[0] >> 11) + 15);
  ExpectTrue("JPEG vertical green gradient", ((decoded.source_bitmap.pixels[56] >> 5) & 63) >
             ((decoded.source_bitmap.pixels[0] >> 5) & 63) + 30);
}

void TestBilinearColorAndEdges() {
  CbzBitmap src;
  src.width = 2; src.height = 2;
  src.pixels = {0xF800, 0x07E0, 0x001F, 0xFFFF}; // red, green, blue, white
  CbzBitmap out;
  ExpectTrue("bilinear upscale", ScaleCbzBitmap(src, 3, 3, true, &out));
  ExpectTrue("bilinear output dimensions", out.width == 3 && out.height == 3 && out.pixels.size() == 9);
  ExpectTrue("bilinear keeps corners", out.pixels[0] == 0xF800 && out.pixels[2] == 0x07E0 &&
             out.pixels[6] == 0x001F && out.pixels[8] == 0xFFFF);
  ExpectTrue("bilinear center averages four colors", out.pixels[4] == 0x8410);
  ExpectTrue("bilinear top edge averages red and green", out.pixels[1] == 0x8400);
  ExpectTrue("bilinear downscale", ScaleCbzBitmap(src, 1, 1, true, &out));
  ExpectTrue("bilinear downscale keeps average", out.pixels.size() == 1 && out.pixels[0] == 0x8410);
  ExpectTrue("invalid target rejected", !ScaleCbzBitmap(src, 0, 3, true, &out));
}

// Compare against the direct coordinate formula across non-integer ratios,
// single-pixel axes, upscales and downscales. Pixel values must not change.
void TestNearestScaleCoordinates() {
  const int sizes[] = {1, 2, 3, 7, 16, 31};
  for (int sw : sizes) for (int sh : sizes) {
    CbzBitmap src;
    src.width = sw; src.height = sh;
    for (int i = 0; i < sw*sh; ++i) src.pixels.push_back((uint16_t)(i*67));
    for (int dw : sizes) for (int dh : sizes) {
      CbzBitmap out;
      ExpectTrue("nearest scale", ScaleCbzBitmap(src, dw, dh, false, &out));
      ExpectTrue("nearest dimensions", out.width == dw && out.height == dh);
      for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x)
        ExpectTrue("nearest exact pixel", out.pixels[y*dw+x] ==
                   src.pixels[(y*sh/dh)*sw+x*sw/dw]);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    Fail("usage: test_cbz_decode <sample.png> <sample.jpg>");
  TestNearestScaleCoordinates();
  TestPngPixelsAndFailureRecovery(argv[1]);
  TestJpegDecode(argv[2]);
  TestBilinearColorAndEdges();
  return 0;
}
