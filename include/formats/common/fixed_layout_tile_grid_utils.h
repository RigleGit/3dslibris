#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fixed_layout_tiles {
struct Rect {
  float left, top, width, height;
};
struct Tile {
  int x, y, width, height, tex_width, tex_height;
};
struct Quad {
  size_t tile;
  float x, y, width, height, u0, v0, u1, v1;
};
// Includes a one-pixel neighbour halo. Hard bounds also limit pathological
// narrow/tall images before allocating a plan or submitting GPU objects.
bool BuildGrid(int width, int height, std::vector<Tile> *tiles, size_t *bytes);
size_t MortonOffset(unsigned x, unsigned y, unsigned tex_width);
// Output is a contiguous range in Morton order, so VRAM uploads need only a
// small reusable linear staging buffer. No complete swizzled copy is needed.
bool SwizzleChunk(const Tile &tile, const uint16_t *pixels, size_t pixel_count,
                  int stride, int width, int height, size_t first, size_t count,
                  uint16_t *out);
bool VisibleQuads(const std::vector<Tile> &tiles, int width, int height,
                  const Rect &source, const Rect &viewport, const Rect &dest,
                  std::vector<Quad> *quads);
} // namespace fixed_layout_tiles
