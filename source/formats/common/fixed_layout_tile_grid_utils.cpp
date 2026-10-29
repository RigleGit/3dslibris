#include "formats/common/fixed_layout_tile_grid_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fixed_layout_tiles {
namespace {
const size_t kMaxBytes = 8u * 1024u * 1024u;
const size_t kMaxTiles = 63; // One further citro2d object is the background.
int Pot(int n) {
  int p = 8;
  while (p < n)
    p *= 2;
  return p;
}
// Keep the usual few large tiles when they fit. If padding exceeds the cache
// budget, decompose remainders into smaller POT interiors before falling back.
size_t CompactRemainder(int n, std::vector<int> *out) {
  const int p = Pot(n + 2);
  if (p == 8) {
    out->push_back(n);
    return p;
  }
  const int first = p / 2 - 2;
  std::vector<int> tail;
  const size_t split_cost = p / 2 + CompactRemainder(n - first, &tail);
  if (split_cost < (size_t)p) {
    out->push_back(first);
    out->insert(out->end(), tail.begin(), tail.end());
    return split_cost;
  }
  out->push_back(n);
  return p;
}
size_t Axis(int n, bool compact, std::vector<int> *out) {
  size_t sum = 0;
  while (n > 1022) {
    out->push_back(1022);
    sum += 1024;
    n -= 1022;
  }
  if (compact)
    sum += CompactRemainder(n, out);
  else {
    out->push_back(n);
    sum += Pot(n + 2);
  }
  return sum;
}
bool Positive(const Rect &r) {
  return std::isfinite(r.left) && std::isfinite(r.top) &&
         std::isfinite(r.width) && std::isfinite(r.height) && r.width > 0 &&
         r.height > 0;
}
} // namespace
bool BuildGrid(int width, int height, std::vector<Tile> *tiles, size_t *bytes) {
  if (!tiles || !bytes)
    return false;
  tiles->clear();
  *bytes = 0;
  if (width <= 0 || height <= 0)
    return false;
  const size_t cols = ((size_t)width + 1021) / 1022;
  const size_t rows = ((size_t)height + 1021) / 1022;
  if (cols > kMaxTiles || rows > kMaxTiles || cols * rows > kMaxTiles)
    return false;
  std::vector<int> widths, heights;
  size_t tex_width = Axis(width, false, &widths);
  size_t tex_height = Axis(height, false, &heights);
  if (tex_width > kMaxBytes / 2 / tex_height) {
    widths.clear();
    heights.clear();
    tex_width = Axis(width, true, &widths);
    tex_height = Axis(height, true, &heights);
  }
  if (widths.size() * heights.size() > kMaxTiles ||
      tex_width > kMaxBytes / 2 / tex_height)
    return false;
  int y = 0;
  for (size_t row = 0; row < heights.size(); ++row) {
    const int h = heights[row];
    int x = 0;
    for (size_t col = 0; col < widths.size(); ++col) {
      const int w = widths[col];
      Tile t = {x, y, w, h, Pot(w + 2), Pot(h + 2)};
      const size_t size = (size_t)t.tex_width * t.tex_height * 2;
      if (size > kMaxBytes - *bytes) {
        tiles->clear();
        *bytes = 0;
        return false;
      }
      *bytes += size;
      tiles->push_back(t);
      x += w;
    }
    y += h;
  }
  return true;
}
size_t MortonOffset(unsigned x, unsigned y, unsigned tex_width) {
  const unsigned low = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                       ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
  return ((size_t)(y >> 3) * (tex_width >> 3) + (x >> 3)) * 64 + low;
}
bool SwizzleChunk(const Tile &t, const uint16_t *pixels, size_t pixel_count,
                  int stride, int width, int height, size_t first, size_t count,
                  uint16_t *out) {
  const size_t total = (size_t)t.tex_width * t.tex_height;
  if (!pixels || !out || width <= 0 || height <= 0 || stride < width ||
      t.tex_width < 8 || t.tex_height < 8 || t.width <= 0 || t.height <= 0 ||
      pixel_count < (size_t)(height - 1) * stride + width || first > total ||
      count > total - first)
    return false;
  const unsigned blocks_per_row = (unsigned)t.tex_width >> 3;
  unsigned row_shift = 0;
  while ((1u << row_shift) < blocks_per_row)
    ++row_shift;
  const unsigned column_mask = blocks_per_row - 1;
  const int last_x = std::min(width - 1, t.x + t.width);
  const int last_y = std::min(height - 1, t.y + t.height);
  const unsigned row_offsets[8] = {0, 2, 8, 10, 32, 34, 40, 42};
  const size_t end = first + count;
  for (size_t index = first; index < end;) {
    const size_t block = index >> 6;
    const int x = (int)(block & column_mask) * 8;
    const int y = (int)(block >> row_shift) * 8;
    const size_t offset = index & 63;
    const size_t n = std::min((size_t)64 - offset, end - index);
    uint16_t partial[64];
    uint16_t *dst = offset == 0 && n == 64 ? out + index - first : partial;
    if (x >= t.width + 2 || y >= t.height + 2) {
      // Visible UVs stop at the halo. Do not sample the source for the rest
      // of the POT allocation; whole unused blocks are deterministic zeroes.
      std::memset(dst, 0, 64 * sizeof(uint16_t));
    } else {
      const int sx = t.x + x - 1;
      const bool interior = sx >= 0 && sx + 7 <= last_x;
      int columns[8];
      if (!interior)
        for (int c = 0; c < 8; ++c)
          columns[c] = std::max(0, std::min(last_x, sx + c));
      for (int r = 0; r < 8; ++r) {
        const int sy = std::max(0, std::min(last_y, t.y + y + r - 1));
        const uint16_t *row = pixels + (size_t)sy * stride;
        uint16_t *d = dst + row_offsets[r];
        // Read each eight-pixel source row consecutively. Its Morton writes
        // stay in one 128-byte block, without per-texel coordinate decoding.
        const uint16_t *p = interior ? row + sx : nullptr;
        d[0] = p ? p[0] : row[columns[0]];
        d[1] = p ? p[1] : row[columns[1]];
        d[4] = p ? p[2] : row[columns[2]];
        d[5] = p ? p[3] : row[columns[3]];
        d[16] = p ? p[4] : row[columns[4]];
        d[17] = p ? p[5] : row[columns[5]];
        d[20] = p ? p[6] : row[columns[6]];
        d[21] = p ? p[7] : row[columns[7]];
      }
    }
    if (dst == partial)
      std::memcpy(out + index - first, partial + offset, n * sizeof(uint16_t));
    index += n;
  }
  return true;
}
bool VisibleQuads(const std::vector<Tile> &tiles, int width, int height,
                  const Rect &source, const Rect &vp, const Rect &dst,
                  std::vector<Quad> *quads) {
  if (!quads)
    return false;
  quads->clear();
  const float eps = 0.000001f;
  if (width <= 0 || height <= 0 || !Positive(source) || !Positive(vp) ||
      !Positive(dst) || vp.left < source.left - eps ||
      vp.top < source.top - eps ||
      vp.left + vp.width > source.left + source.width + eps ||
      vp.top + vp.height > source.top + source.height + eps)
    return false;
  const float left = (vp.left - source.left) / source.width * width;
  const float top = (vp.top - source.top) / source.height * height;
  const float w = vp.width / source.width * width,
              h = vp.height / source.height * height;
  for (size_t i = 0; i < tiles.size(); ++i) {
    const Tile &t = tiles[i];
    const float x0 = std::max(left, (float)t.x), y0 = std::max(top, (float)t.y);
    const float x1 = std::min(left + w, (float)(t.x + t.width));
    const float y1 = std::min(top + h, (float)(t.y + t.height));
    if (x1 <= x0 || y1 <= y0)
      continue;
    const float dx0 = dst.left + (x0 - left) / w * dst.width;
    const float dy0 = dst.top + (y0 - top) / h * dst.height;
    const float dx1 = dst.left + (x1 - left) / w * dst.width;
    const float dy1 = dst.top + (y1 - top) / h * dst.height;
    // Tex3DS convention: row zero is v=1. RGB565 bytes remain unmodified.
    Quad q = {i,
              dx0,
              dy0,
              dx1 - dx0,
              dy1 - dy0,
              (x0 - t.x + 1) / t.tex_width,
              1 - (y0 - t.y + 1) / t.tex_height,
              (x1 - t.x + 1) / t.tex_width,
              1 - (y1 - t.y + 1) / t.tex_height};
    quads->push_back(q);
  }
  return !quads->empty();
}
} // namespace fixed_layout_tiles
