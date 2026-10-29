#pragma once
#include "formats/common/fixed_layout_tile_grid_utils.h"
#include <cstddef>
#include <cstdint>
class Text;
extern unsigned viewer_gpu_upload_attempts, viewer_gpu_resets;
extern bool viewer_gpu_available, viewer_gpu_queue_ok;
namespace fixed_layout_gpu {
// Controllable hardware boundary; the real controller owns source validity,
// CPU pixel release, fallback reconstruction and input behavior.
struct TextureCache {
  bool resident = false;
  uint64_t generation = 0;
  bool Matches(uint64_t value) const { return resident && generation == value; }
  bool Upload(Text *, const uint16_t *pixels, size_t, int, int, int, uint64_t value) {
    if (Matches(value)) return true;
    ++viewer_gpu_upload_attempts;
    resident = viewer_gpu_available && pixels;
    generation = value;
    return resident;
  }
  void Reset() { ++viewer_gpu_resets; resident = false; }
};
inline bool Queue(Text *, TextureCache &cache, const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &) {
  return cache.resident && viewer_gpu_queue_ok;
}
} // namespace fixed_layout_gpu
