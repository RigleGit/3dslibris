#pragma once
#include "formats/common/fixed_layout_tile_grid_utils.h"
#include <cstddef>
#include <cstdint>
class Text;
extern unsigned viewer_gpu_upload_attempts;
namespace fixed_layout_gpu {
// Hardware boundary for controller tests. Allocation is unavailable, but every
// attempted upload is visible so loading/suspend cannot silently allocate.
struct TextureCache {
  bool Upload(Text *, const uint16_t *, size_t, int, int, int, uint64_t) {
    ++viewer_gpu_upload_attempts;
    return false;
  }
  void Reset() {}
};
inline bool Queue(Text *, TextureCache &, const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &,
                  const fixed_layout_tiles::Rect &) {
  return false;
}
} // namespace fixed_layout_gpu
