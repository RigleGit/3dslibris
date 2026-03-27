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

void TestDecodePath(const char *path) {
  const std::vector<unsigned char> bytes = ReadFile(path);
  CbzDecodedPage decoded;
  ExpectTrue("decode", DecodeCbzPageImage(bytes, 5, &decoded));
  ExpectTrue("original width", decoded.original_width > 0);
  ExpectTrue("original height", decoded.original_height > 0);
  ExpectTrue("scaled width", decoded.source_bitmap.width > 0);
  ExpectTrue("scaled height", decoded.source_bitmap.height > 0);
  ExpectTrue("pixels not empty", !decoded.source_bitmap.pixels.empty());

  CbzBitmap scaled;
  ExpectTrue("rescale", ScaleCbzBitmap(decoded.source_bitmap, 4, 4, true, &scaled));
  ExpectTrue("scaled pixels not empty", !scaled.pixels.empty());
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    Fail("usage: test_cbz_decode <sample.png> <sample.jpg>");
  TestDecodePath(argv[1]);
  TestDecodePath(argv[2]);
  return 0;
}
