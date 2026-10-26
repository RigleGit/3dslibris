#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shared/orientation_utils.h"

namespace framebuffer_blit_utils {

struct DirtyRect {
  int x0;
  int y0;
  int x1;
  int y1;
  bool valid;
};

struct FramebufferGeometry {
  int stride;
  int phys_width;
  size_t byte_size;
};

struct PhysicalFramebufferSyncState {
  const uint8_t *slots[2];
  uint64_t slot_generations[2];

  PhysicalFramebufferSyncState() : slots{nullptr, nullptr}, slot_generations{0, 0} {}
};

inline int LogicalTextScreenHeight(bool is_left_screen) {
  return is_left_screen ? 400 : 320;
}

inline size_t LogicalTextScreenPixelCount(int logical_width,
                                          bool is_left_screen) {
  if (logical_width <= 0)
    return 0;
  return (size_t)logical_width *
         (size_t)LogicalTextScreenHeight(is_left_screen);
}

inline FramebufferGeometry MakeFramebufferGeometry(int fb_width, int fb_height) {
  FramebufferGeometry geometry;
  geometry.stride = std::min(fb_width, fb_height);
  geometry.phys_width = std::max(fb_width, fb_height);
  geometry.byte_size = (size_t)fb_width * (size_t)fb_height * 3u;
  return geometry;
}

inline int ResolvePhysicalFramebufferSlot(PhysicalFramebufferSyncState *state,
                                          const uint8_t *fb) {
  if (!state || !fb)
    return -1;

  for (int i = 0; i < 2; ++i) {
    if (state->slots[i] == fb)
      return i;
  }
  for (int i = 0; i < 2; ++i) {
    if (!state->slots[i]) {
      state->slots[i] = fb;
      state->slot_generations[i] = 0;
      return i;
    }
  }

  state->slots[0] = fb;
  state->slot_generations[0] = 0;
  return 0;
}

inline bool NeedsPhysicalFramebufferCopy(
    const PhysicalFramebufferSyncState &state, int slot,
    uint64_t cache_generation) {
  if (slot < 0 || slot >= 2)
    return true;
  return !state.slots[slot] || state.slot_generations[slot] != cache_generation;
}

inline void MarkPhysicalFramebufferCopied(PhysicalFramebufferSyncState *state,
                                          int slot, const uint8_t *fb,
                                          uint64_t cache_generation) {
  if (!state || slot < 0 || slot >= 2 || !fb)
    return;
  state->slots[slot] = fb;
  state->slot_generations[slot] = cache_generation;
}

inline DirtyRect MakeDirtyRect(int x0, int y0, int x1, int y1) {
  DirtyRect rect = {x0, y0, x1, y1, x0 < x1 && y0 < y1};
  return rect;
}

inline void ExpandDirtyRect(DirtyRect *dst, int x0, int y0, int x1, int y1,
                            int max_width, int max_height) {
  if (!dst || max_width <= 0 || max_height <= 0)
    return;

  x0 = std::max(0, std::min(max_width, x0));
  y0 = std::max(0, std::min(max_height, y0));
  x1 = std::max(0, std::min(max_width, x1));
  y1 = std::max(0, std::min(max_height, y1));
  if (x0 >= x1 || y0 >= y1)
    return;

  if (!dst->valid) {
    *dst = MakeDirtyRect(x0, y0, x1, y1);
    return;
  }

  dst->x0 = std::min(dst->x0, x0);
  dst->y0 = std::min(dst->y0, y0);
  dst->x1 = std::max(dst->x1, x1);
  dst->y1 = std::max(dst->y1, y1);
  dst->valid = true;
}

// Maps a logical pixel to its byte offset in the physical framebuffer, whose
// layout follows the libctru convention: column-major with `stride` (the short
// 240px axis) per column. Portrait orientations rotate the logical page 90°
// onto the panel; landscape only flips the vertical axis.
inline size_t PhysicalOffsetBytes(const FramebufferGeometry &geometry, int sx,
                                  int sy, unsigned char orientation) {
  int dx, dy;
  if (orientation_utils::IsLandscape(orientation)) {
    dx = sx;
    dy = geometry.stride - 1 - sy;
  } else if (orientation_utils::IsTurnedRight(orientation)) {
    dx = sy;
    dy = sx;
  } else {
    dx = geometry.phys_width - 1 - sy;
    dy = geometry.stride - 1 - sx;
  }
  return ((size_t)dx * (size_t)geometry.stride + (size_t)dy) * 3u;
}

// Logical x spans the long physical axis in landscape and the short one in
// portrait; logical y spans the other. Clamp accordingly so wide landscape
// rows are not truncated at the 240px stride.
inline int MaxConvertibleLogicalX(const FramebufferGeometry &geometry,
                                  int logical_width,
                                  unsigned char orientation) {
  const int limit = orientation_utils::IsLandscape(orientation)
                        ? geometry.phys_width
                        : geometry.stride;
  return std::min(logical_width, limit);
}

inline int MaxConvertibleLogicalY(const FramebufferGeometry &geometry,
                                  int logical_height,
                                  unsigned char orientation) {
  const int limit = orientation_utils::IsLandscape(orientation)
                        ? geometry.stride
                        : geometry.phys_width;
  return std::min(logical_height, limit);
}

// Wide mode doubles physical horizontal density without changing UI geometry.
// Native wide bitmaps already span 800 samples and are copied one-for-one.
inline void StoreLogicalPixel(uint8_t *dst, const FramebufferGeometry &geometry,
                               uint16_t pixel, int sx, int sy,
                               int logical_width, int logical_height,
                               unsigned char orientation, bool preserve_white = false) {
  const bool landscape = orientation_utils::IsLandscape(orientation);
  const int long_axis = landscape ? logical_width : logical_height;
  const int density = geometry.phys_width == 800 && long_axis == 400 ? 2 : 1;
  for (int sample = 0; sample < density; ++sample) {
    const int px = landscape ? sx * density + sample : sx;
    const int py = landscape ? sy : sy * density + sample;
    const size_t off = PhysicalOffsetBytes(geometry, px, py, orientation);
    if (preserve_white && pixel == 0xFFFF) {
      dst[off] = dst[off + 1] = dst[off + 2] = 0xFF;
      continue;
    }
    dst[off] = (uint8_t)((pixel & 0x1F) << 3);
    dst[off + 1] = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
    dst[off + 2] = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
  }
}

inline void ConvertLogicalRgb565ToPhysicalBgr888(
    uint8_t *dst, const FramebufferGeometry &geometry, const uint16_t *src,
    int src_stride, int logical_width, int logical_height,
    unsigned char orientation) {
  if (!dst || !src || geometry.stride <= 0 || geometry.phys_width <= 0 ||
      geometry.byte_size == 0 || src_stride <= 0 || logical_width <= 0 ||
      logical_height <= 0) {
    return;
  }

  std::memset(dst, 0xFF, geometry.byte_size);

  const int max_sy = MaxConvertibleLogicalY(geometry, logical_height, orientation);
  const int max_sx = MaxConvertibleLogicalX(geometry, logical_width, orientation);
  const bool double_density = geometry.phys_width == 800 &&
      (orientation_utils::IsLandscape(orientation) ? logical_width : logical_height) == 400;
  if (!double_density) {
    // Keep the ordinary/native path direct. At -Og devkitARM leaves a
    // density-helper call per pixel, penalising 400px presentation.
    for (int sy = 0; sy < max_sy; ++sy) {
      for (int sx = 0; sx < max_sx; ++sx) {
        const uint16_t pixel = src[(size_t)sy * (size_t)src_stride + (size_t)sx];
        const size_t off = PhysicalOffsetBytes(geometry, sx, sy, orientation);
        dst[off] = (uint8_t)((pixel & 0x1F) << 3);
        dst[off + 1] = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
        dst[off + 2] = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
      }
    }
    return;
  }
  for (int sy = 0; sy < max_sy; sy++) {
    for (int sx = 0; sx < max_sx; sx++) {
      const uint16_t pixel = src[(size_t)sy * (size_t)src_stride + (size_t)sx];
      StoreLogicalPixel(dst, geometry, pixel, sx, sy, logical_width,
                         logical_height, orientation);
    }
  }
}

inline void ConvertLogicalRgb565RectToPhysicalBgr888(
    uint8_t *dst, const FramebufferGeometry &geometry, const uint16_t *src,
    int src_stride, int logical_width, int logical_height,
    unsigned char orientation, const DirtyRect &dirty) {
  if (!dst || !src || !dirty.valid || geometry.stride <= 0 ||
      geometry.phys_width <= 0 || geometry.byte_size == 0 || src_stride <= 0 ||
      logical_width <= 0 || logical_height <= 0) {
    return;
  }

  const int max_sy = MaxConvertibleLogicalY(geometry, logical_height, orientation);
  const int max_sx = MaxConvertibleLogicalX(geometry, logical_width, orientation);
  const int y0 = std::max(0, std::min(max_sy, dirty.y0));
  const int y1 = std::max(0, std::min(max_sy, dirty.y1));
  const int x0 = std::max(0, std::min(max_sx, dirty.x0));
  const int x1 = std::max(0, std::min(max_sx, dirty.x1));
  if (x0 >= x1 || y0 >= y1)
    return;

  const bool double_density = geometry.phys_width == 800 &&
      (orientation_utils::IsLandscape(orientation) ? logical_width : logical_height) == 400;
  if (!double_density) {
    for (int sy = y0; sy < y1; ++sy) {
      for (int sx = x0; sx < x1; ++sx) {
        const uint16_t pixel = src[(size_t)sy * (size_t)src_stride + (size_t)sx];
        const size_t off = PhysicalOffsetBytes(geometry, sx, sy, orientation);
        if (pixel == 0xFFFF) {
          dst[off] = dst[off + 1] = dst[off + 2] = 0xFF;
        } else {
          dst[off] = (uint8_t)((pixel & 0x1F) << 3);
          dst[off + 1] = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
          dst[off + 2] = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
        }
      }
    }
    return;
  }

  for (int sy = y0; sy < y1; sy++) {
    for (int sx = x0; sx < x1; sx++) {
      const uint16_t pixel = src[(size_t)sy * (size_t)src_stride + (size_t)sx];
      StoreLogicalPixel(dst, geometry, pixel, sx, sy, logical_width,
                         logical_height, orientation, true);
    }
  }
}

} // namespace framebuffer_blit_utils
