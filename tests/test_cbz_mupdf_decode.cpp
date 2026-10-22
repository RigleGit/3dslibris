#include "formats/cbz/cbz_decode.h"
#include "test_assert.h"

extern "C" {
#include "mupdf/fitz/buffer.h"
#include "mupdf/fitz/context.h"
}

#include <fstream>
#include <iterator>
#include <vector>

namespace {
bool input_is_borrowed = false;
size_t buffer_bytes = 0;
unsigned buffer_calls = 0;

void NoLock(void *, int) {}

fz_buffer *RecordStorage(fz_context *ctx, fz_buffer *buffer,
                         const unsigned char *input, size_t size) {
  unsigned char *storage = nullptr;
  buffer_bytes = fz_buffer_storage(ctx, buffer, &storage);
  input_is_borrowed = storage == input && buffer_bytes == size;
  ++buffer_calls;
  return buffer;
}

std::vector<unsigned char> ReadFile(const char *path) {
  std::ifstream file(path, std::ios::binary);
  test::ExpectTrue("MuPDF image fixture opens", file.good());
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
}

void DecodeAndCheckInput(std::vector<unsigned char> &bytes, bool expected_ok,
                         CbzDecodedPage *page) {
  const std::vector<unsigned char> before = bytes;
  buffer_calls = 0;
  buffer_bytes = 0;
  input_is_borrowed = false;
  test::ExpectTrue("MuPDF decode result", DecodeCbzPageImage(bytes, 5, 240, 400, page) == expected_ok);
  test::ExpectTrue("MuPDF cleanup leaves caller bytes intact", bytes == before);
  if (!bytes.empty()) {
    test::ExpectEqU("one compressed buffer header per decode", buffer_calls, 1);
    test::ExpectEqU64("buffer spans original compressed entry", buffer_bytes, bytes.size());
    test::ExpectTrue("MuPDF buffer borrows actual caller storage without a copy", input_is_borrowed);
  }
  if (!expected_ok) {
    test::ExpectEq("failed MuPDF decode clears width", page->original_width, 0);
    test::ExpectEq("failed MuPDF decode clears height", page->original_height, 0);
    test::ExpectTrue("failed MuPDF decode clears bitmap", page->source_bitmap.pixels.empty());
    test::ExpectTrue("failed MuPDF decode supplies diagnostic", GetLastCbzDecodeError()[0] != '\0');
  } else {
    test::ExpectTrue("MuPDF recovery clears diagnostic", GetLastCbzDecodeError()[0] == '\0');
    test::ExpectEq("MuPDF source width", page->original_width, 8);
    test::ExpectEq("MuPDF source height", page->original_height, 8);
    test::ExpectEq("MuPDF bitmap width", page->source_bitmap.width, 8);
    test::ExpectEq("MuPDF bitmap height", page->source_bitmap.height, 8);
    test::ExpectEqU64("MuPDF RGB565 sample count", page->source_bitmap.pixels.size(), 64);
  }
}
}

// Only the platform lock boundary is substituted. Buffers/images/pixmaps use
// the real bundled MuPDF library, including its own error and cleanup paths.
fz_locks_context g_mupdf_locks_ctx = {nullptr, NoLock, NoLock};
void InitMuPdfLocks() {}

// The runner interposes only these application calls, delegates to real MuPDF,
// and observes its returned storage. No decoder behavior is simulated.
extern "C" fz_buffer *TestMuPdfCopiedBuffer(fz_context *ctx, const unsigned char *data, size_t size) {
  return RecordStorage(ctx, fz_new_buffer_from_copied_data(ctx, data, size), data, size);
}
extern "C" fz_buffer *TestMuPdfSharedBuffer(fz_context *ctx, const unsigned char *data, size_t size) {
  return RecordStorage(ctx, fz_new_buffer_from_shared_data(ctx, data, size), data, size);
}

int main(int argc, char **argv) {
  test::ExpectEq("PNG and JPEG fixture paths supplied", argc, 3);
  std::vector<unsigned char> png = ReadFile(argv[1]);
  std::vector<unsigned char> jpeg = ReadFile(argv[2]);
  CbzDecodedPage page;
  DecodeAndCheckInput(png, true, &page);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
    const uint16_t expected = (uint16_t)((x * 4 << 11) | (y * 8 << 5) | 20);
    test::ExpectEq("real MuPDF PNG RGB565 pixel", page.source_bitmap.pixels[y * 8 + x], expected);
  }
  std::vector<std::vector<unsigned char>> invalids = {
      {}, std::vector<unsigned char>(12, 'x'),
      std::vector<unsigned char>(png.begin(), png.begin() + 8)};
  for (auto &invalid : invalids) {
    DecodeAndCheckInput(invalid, false, &page);
    DecodeAndCheckInput(png, true, &page);
  }
  for (int i = 0; i < 20; ++i) {
    // Each source is destroyed before inspecting the returned bitmap.
    {
      std::vector<unsigned char> entry = (i % 2) ? jpeg : png;
      DecodeAndCheckInput(entry, true, &page);
    }
    test::ExpectTrue("decoded bitmap survives source destruction", (page.source_bitmap.pixels[7] >> 11) >
                     (page.source_bitmap.pixels[0] >> 11) + 15);
    test::ExpectTrue("JPEG/PNG preserves vertical gradient", ((page.source_bitmap.pixels[56] >> 5) & 63) >
                     ((page.source_bitmap.pixels[0] >> 5) & 63) + 30);
  }
  puts("PASS: real MuPDF CBZ borrowed input, pixels, errors and repeated cleanup");
}
