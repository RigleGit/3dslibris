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
  bool UploadRgb(Text *text, const raster_image::Rgb8View &source, uint64_t generation);
  void Reset();
#else
  ~TextureCache() {}
  TextureCache(TextureCache &&) = default;
  TextureCache &operator=(TextureCache &&) = default;
  bool Upload(Text *, const uint16_t *, size_t, int, int, int, uint64_t) {
    return false;
  }
  bool UploadRgb(Text *, const raster_image::Rgb8View &, uint64_t) { return false; }
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
#ifdef __3DS__
  bool UploadImpl(Text *, const uint16_t *, size_t, int, int, int, uint64_t,
                  const raster_image::Rgb8View *);
#endif
  friend struct Renderer;
  struct Impl;
  Impl *impl_;
  uint64_t generation_;
  bool attempted_;
};

// One power-of-two RGB565 texture (a mosaic tile). Move-only; main thread only.
// Reset withdraws any queued, not yet presented, draw that references it.
class TileTexture {
public:
  TileTexture() : impl_(nullptr) {}
  TileTexture(const TileTexture &) = delete;
  TileTexture &operator=(const TileTexture &) = delete;
#ifdef __3DS__
  ~TileTexture();
  TileTexture(TileTexture &&other);
  TileTexture &operator=(TileTexture &&other);
  // VRAM first, linear memory as fallback. Width/height: power of two >= 8.
  bool Create(Text *text, int width, int height);
  // Hardware path: `linear` is a 128-byte-aligned width*height RGB8 buffer
  // (B,G,R byte order, row 0 = texture row 0). One GX display transfer
  // converts it to tiled RGB565; the buffer is reusable when this returns.
  bool UploadHardware(const void *linear, int width, int height);
  // CPU path from an R,G,B ordered buffer of the same size.
  bool UploadRgb8(const raster_image::Rgb8View &source);
  void Reset();
#else
  ~TileTexture() {}
  TileTexture(TileTexture &&) = default;
  TileTexture &operator=(TileTexture &&) = default;
  bool Create(Text *, int, int) { return false; }
  bool UploadHardware(const void *, int, int) { return false; }
  bool UploadRgb8(const raster_image::Rgb8View &) { return false; }
  void Reset() { impl_ = nullptr; }
#endif
  bool Valid() const { return impl_ != nullptr; }
  struct Impl;
  Impl *impl_;
};

struct TileDraw {
  const TileTexture *texture;
  int interior_width, interior_height;
  float x, y, width, height, u0, v0, u1, v1;
};

#ifdef __3DS__
// Linear scratch for tile rendering (main thread). Null when linear memory is
// short; the caller must then use the non-GPU path.
void *AllocLinearScratch(size_t bytes);
void FreeLinearScratch(void *);
// Queues tile quads for the next Present. `append` adds to a frame already
// begun by Queue (the underlay); otherwise it starts a new frame.
bool QueueTiles(Text *, const void *owner, const TileDraw *draws, size_t count,
                bool append);
#else
inline void *AllocLinearScratch(size_t) { return nullptr; }
inline void FreeLinearScratch(void *) {}
inline bool QueueTiles(Text *, const void *, const TileDraw *, size_t, bool) {
  return false;
}
#endif

#ifdef __3DS__
// All entry points are main-thread only. Queue borrows the cache only until
// Present; Reset withdraws that borrow before freeing any texture.
bool Queue(Text *, TextureCache &, const fixed_layout_tiles::Rect &source,
           const fixed_layout_tiles::Rect &viewport,
           const fixed_layout_tiles::Rect &destination, int quarter_turns = 0);
bool Active(const Text *);
bool WillDraw(const Text *);
bool NeedsPresent(Text *);
bool Present(Text *);
void Software(Text *, bool discard = false);
void Shutdown(Text *);
#else
inline bool Queue(Text *, TextureCache &, const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &, int = 0) {
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
