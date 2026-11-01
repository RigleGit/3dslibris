#pragma once
#include <cstddef>
#include <limits>
#include <cstdlib>
namespace cache_memory {
// Probe contiguous allocator headroom, including heap growth. Region-free and
// mallinfo alone omit either the reserved heap or its not-yet-used tail.
inline bool HasHeadroom(size_t bytes, size_t reserve = 2u * 1024u * 1024u) {
  if (bytes > std::numeric_limits<size_t>::max() - reserve)
    return false;
  void *probe = std::malloc(bytes + reserve);
  if (!probe)
    return false;
  std::free(probe);
  return true;
}
} // namespace cache_memory
