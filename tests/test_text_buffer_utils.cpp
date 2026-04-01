#include "ui/text_buffer_utils.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::fprintf(stderr, "%s\n", msg);
  std::exit(1);
}

void TestFillLogicalScreenRowsUsesStride() {
  const int stride = 400;
  const int width = 240;
  const int logical_height = 400;
  const uint16_t color = 0x1234;

  std::vector<uint16_t> buffer((size_t)stride * (size_t)logical_height, 0xFFFF);
  text_buffer_utils::FillLogicalScreenRows(buffer.data(), stride, width,
                                           logical_height, color);

  if (buffer[(size_t)399 * (size_t)stride + 0] != color)
    Fail("last row visible pixel not cleared");
  if (buffer[(size_t)399 * (size_t)stride + 239] != color)
    Fail("last row last visible pixel not cleared");
  if (buffer[(size_t)399 * (size_t)stride + 240] != 0xFFFF)
    Fail("stride padding should remain untouched");
  if (buffer[(size_t)240 * (size_t)stride + 0] != color)
    Fail("rows beyond contiguous 240*400 block must be cleared");
}

} // namespace

int main() {
  TestFillLogicalScreenRowsUsesStride();
  return 0;
}
