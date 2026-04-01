#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace page_buffer_utils {

struct OwnedPageBuffer {
  std::vector<uint8_t> bytes;
};

inline size_t RequiredPageBufferCapacity(size_t current_capacity,
                                         size_t required_length) {
  if (required_length == 0)
    return 0;
  if (current_capacity >= required_length)
    return current_capacity;
  return required_length;
}

inline size_t RequiredPageVectorCapacity(size_t current_count,
                                         size_t current_capacity,
                                         size_t incoming_pages) {
  const size_t required_count = current_count + incoming_pages;
  if (current_capacity >= required_count)
    return current_capacity;
  return required_count;
}

inline OwnedPageBuffer AdoptPageBuffer(std::vector<uint8_t> *src) {
  OwnedPageBuffer out;
  if (src)
    out.bytes.swap(*src);
  return out;
}

} // namespace page_buffer_utils
