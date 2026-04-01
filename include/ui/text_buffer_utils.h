#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace text_buffer_utils {

inline void FillLogicalScreenRows(uint16_t *buffer, int stride, int width,
                                  int logical_height, uint16_t color) {
  if (!buffer || stride <= 0 || width <= 0 || logical_height <= 0)
    return;

  for (int y = 0; y < logical_height; y++) {
    std::fill(buffer + (size_t)y * (size_t)stride,
              buffer + (size_t)y * (size_t)stride + (size_t)width, color);
  }
}

} // namespace text_buffer_utils
