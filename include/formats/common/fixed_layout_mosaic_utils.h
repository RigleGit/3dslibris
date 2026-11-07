#pragma once
// Pure tile-grid math for the zoomed fixed-layout mosaic. Tiles are 256x256
// textures holding a 254x254 interior plus a one-pixel halo of the neighbouring
// device pixels, so bilinear sampling across a tile border reads real
// neighbour data instead of a clamped edge texel.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fixed_layout_mosaic {
const int kInterior = 254;
const int kTexture = 256;

struct Grid {
  int cols, rows;
  int width, height; // whole page in device pixels at the epoch's scale
};
struct PixelRect {
  int x, y, w, h; // interior of one tile in device pixels
};
struct Range { // half-open tile range
  int c0, r0, c1, r1;
  bool Empty() const { return c1 <= c0 || r1 <= r0; }
  int Count() const { return Empty() ? 0 : (c1 - c0) * (r1 - r0); }
  bool Contains(int c, int r) const {
    return c >= c0 && c < c1 && r >= r0 && r < r1;
  }
};
struct Coord {
  int col, row;
};
struct Quad {
  float x, y, w, h, u0, v0, u1, v1;
};
struct Entry {
  int col, row, epoch;
  uint32_t stamp;
  size_t bytes;
};

// Smallest power of two >= interior + 2 (halo), at least 8.
int PaddedDim(int interior);
// Fails for non-positive or absurd (more than 64 tiles per axis) pages.
bool MakeGrid(float page_width_px, float page_height_px, Grid *out);
PixelRect Interior(const Grid &grid, int col, int row);
// Tiles intersecting the device-pixel rect [x0,x1) x [y0,y1), grown by `margin`
// tiles on every side and clamped to the grid.
Range TilesFor(const Grid &grid, float x0, float y0, float x1, float y1,
               int margin);
// `inner` tiles first, then the rest of `outer`; each group nearest to (cx,cy)
// (device pixels) first.
void OrderByDistance(const Grid &grid, const Range &inner, const Range &outer,
                     float cx, float cy, std::vector<Coord> *out);
// Visible part of one tile as a destination quad. (vx0..vy1) is the viewport in
// the same device pixels as `interior`; (dx,dy,dw,dh) is where the viewport is
// drawn. Edges shared by two tiles produce bit-identical coordinates.
bool MakeQuad(const PixelRect &interior, int tex_w, int tex_h, float vx0,
              float vy0, float vx1, float vy1, float dx, float dy, float dw,
              float dh, Quad *out);
// Eviction choice: any tile of a superseded epoch first (oldest epoch, then
// oldest use); otherwise the current-epoch tile outside `keep` farthest from
// (cx_tile, cy_tile) in tile units. -1 when nothing may be evicted.
int PickVictim(const std::vector<Entry> &entries, int current_epoch,
               const Range &keep, float cx_tile, float cy_tile);
// Whether to retain a display list of `list_bytes` (0 = unknown) when
// `heap_free` bytes remain: needs room for the next page's list plus a reserve.
bool ShouldKeepDisplayList(size_t heap_free, size_t list_bytes, bool is_new_3ds);
} // namespace fixed_layout_mosaic
