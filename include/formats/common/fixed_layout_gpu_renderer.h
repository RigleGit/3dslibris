#pragma once
#include "formats/common/fixed_layout_tile_grid_utils.h"
#include <cstddef>
#include <cstdint>
class Text;

namespace fixed_layout_gpu {
struct Renderer;
class TextureCache {
public:
  TextureCache() : impl_(nullptr), generation_(0), attempted_(false) {}
  TextureCache(const TextureCache &) = delete;
  TextureCache &operator=(const TextureCache &) = delete;
#ifdef __3DS__
  ~TextureCache();
  TextureCache(TextureCache &&other);
  TextureCache &operator=(TextureCache &&other);
  bool Upload(Text *text, const uint16_t *pixels, size_t count, int width,
              int height, int stride, uint64_t generation);
  void Reset();
#else
  ~TextureCache() {}
  TextureCache(TextureCache &&) = default;
  TextureCache &operator=(TextureCache &&) = default;
  bool Upload(Text *, const uint16_t *, size_t, int, int, int, uint64_t) {
    return false;
  }
  void Reset() {
    impl_ = nullptr;
    attempted_ = false;
  }
#endif
  bool Valid() const {
    return impl_ != nullptr;
  }
  bool Matches(uint64_t generation) const {
    return Valid() && generation_ == generation;
  }

private:
  friend struct Renderer;
  struct Impl;
  Impl *impl_;
  uint64_t generation_;
  bool attempted_;
};

#ifdef __3DS__
// All entry points are main-thread only. Queue borrows the cache only until
// Present; Reset withdraws that borrow before freeing any texture.
bool Queue(Text *, TextureCache &, const fixed_layout_tiles::Rect &source,
           const fixed_layout_tiles::Rect &viewport,
           const fixed_layout_tiles::Rect &destination);
bool Active(const Text *);
bool WillDraw(const Text *);
bool NeedsPresent(Text *);
bool Present(Text *);
void Software(Text *, bool discard = false);
void Shutdown(Text *);
#else
inline bool Queue(Text *, TextureCache &, const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &) {
  return false;
}
inline bool Active(const Text *) {
  return false;
}
inline bool WillDraw(const Text *) {
  return false;
}
inline bool NeedsPresent(Text *) {
  return false;
}
inline bool Present(Text *) {
  return false;
}
inline void Software(Text *, bool = false) {}
inline void Shutdown(Text *) {}
#endif
} // namespace fixed_layout_gpu
