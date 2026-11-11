#pragma once
#include <cstddef>
#include <limits>
#include <cstdlib>
#include <algorithm>
#include <string>
namespace cache_memory {
// Probe contiguous allocator headroom, including heap growth. Region-free and
// mallinfo alone omit either the reserved heap or its not-yet-used tail.
inline bool HasHeadroom(size_t bytes, size_t reserve = 2u * 1024u * 1024u) {
  if (bytes > std::numeric_limits<size_t>::max() - reserve)
    return false;
  void *probe = std::malloc(bytes + reserve);
#if defined(__GNUC__) || defined(__clang__)
  // Without this the compiler folds malloc/free of an unused pointer into
  // "return true" (observed with devkitARM -O2), turning the probe into a no-op.
  __asm__ __volatile__("" : : "r"(probe) : "memory");
#endif
  if (!probe)
    return false;
  std::free(probe);
  return true;
}
// Check only when an append can grow the string. Probing every chunk would
// add allocator traffic even while the existing capacity is sufficient.
inline bool CanAppend(const std::string &text, size_t bytes) {
  if (bytes > text.max_size() - text.size())
    return false;
  const size_t required = text.size() + bytes;
  if (required <= text.capacity())
    return true;
  const size_t doubled =
      text.capacity() > text.max_size() / 2 ? required : text.capacity() * 2;
  return HasHeadroom(std::max(required, doubled), 512u * 1024u);
}
} // namespace cache_memory
