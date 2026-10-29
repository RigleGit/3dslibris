#include "formats/common/fixed_layout_tile_grid_utils.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <climits>
#include <vector>
using namespace fixed_layout_tiles;
int main() {
  std::vector<Tile> tiles;
  size_t bytes = 0;
  assert(BuildGrid(960, 2880, &tiles, &bytes));
  assert(tiles.size() == 3 && bytes == 6u * 1024 * 1024);
  assert(tiles[0].width == 960 && tiles[0].height == 1022);
  assert(tiles[2].y == 2044 && tiles[2].height == 836);
  assert(BuildGrid(1, 1, &tiles, &bytes));
  assert(tiles.size() == 1 && tiles[0].tex_width == 8 && bytes == 128);
  assert(!BuildGrid(0, 4, &tiles, &bytes));
  assert(!BuildGrid(INT_MAX, INT_MAX, &tiles, &bytes));
  // A 6 MiB raster must not fall back merely because a 514px remainder
  // was padded to 1024. Split it into 510+4, including both neighbour halos.
  assert(BuildGrid(2048, 1536, &tiles, &bytes));
  assert(bytes == (1024u + 1024u + 8u) * (1024u + 512u + 8u) * 2);
  assert(tiles.size() == 9);
  size_t covered = 0;
  for (const Tile &t : tiles) {
    assert(t.tex_width >= t.width + 2 && t.tex_height >= t.height + 2);
    covered += (size_t)t.width * t.height;
  }
  assert(covered == 2048u * 1536u);
  assert(
      !BuildGrid(2048, 2048, &tiles, &bytes)); // Halos still count in the cap.
  // Independent bit-interleave oracle, including rectangular texture rows.
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      unsigned low = 0;
      for (unsigned b = 0; b < 3; ++b)
        low |= ((x >> b) & 1) << (2 * b), low |= ((y >> b) & 1) << (2 * b + 1);
      assert(MortonOffset(x, y, 16) == ((y / 8) * 2 + x / 8) * 64 + low);
    }
  assert(BuildGrid(1023, 3, &tiles, &bytes));
  assert(tiles.size() == 2);
  std::vector<uint16_t> image(1023 * 3);
  for (unsigned i = 0; i < image.size(); ++i)
    image[i] = (uint16_t)i;
  for (const Tile &t : tiles) {
    std::vector<uint16_t> out(t.tex_width * t.tex_height);
    assert(SwizzleChunk(t, image.data(), image.size(), 1023, 1023, 3, 0,
                        out.size(), out.data()));
    std::vector<uint16_t> streamed(out.size());
    for (size_t first = 0; first < out.size(); first += 73) {
      const size_t count = std::min((size_t)73, out.size() - first);
      assert(SwizzleChunk(t, image.data(), image.size(), 1023, 1023, 3, first,
                          count, streamed.data() + first));
    }
    assert(streamed == out);
    // Halo samples a neighbour, padding and outer edges clamp to the page.
    for (int y = 0; y < t.tex_height; ++y)
      for (int x = 0; x < t.tex_width; ++x) {
        int sx =
            std::max(0, std::min(1022, t.x + std::min(x, t.width + 1) - 1));
        int sy = std::max(0, std::min(2, t.y + std::min(y, t.height + 1) - 1));
        assert(out[MortonOffset(x, y, t.tex_width)] == image[sy * 1023 + sx]);
      }
  }
  assert(BuildGrid(1800, 1800, &tiles, &bytes));
  Rect source = {0, 0, 1, 1}, viewport = {0.4f, 0.4f, 0.3f, 0.3f};
  Rect dst = {7, 11, 237, 391};
  std::vector<Quad> quads;
  assert(VisibleQuads(tiles, 1800, 1800, source, viewport, dst, &quads));
  assert(quads.size() == 4);
  assert(quads[0].x + quads[0].width == quads[1].x);
  assert(quads[0].y + quads[0].height == quads[2].y);
  assert(std::fabs(quads[3].x + quads[3].width - 244) < 0.001f);
  // A cropped PDF region is mapped in page coordinates, not as a full page.
  source = {0.25f, 0.25f, 0.5f, 0.5f};
  viewport = {0.3f, 0.3f, 0.2f, 0.2f};
  assert(VisibleQuads(tiles, 1800, 1800, source, viewport, dst, &quads));
  viewport = {0, 0, 1, 1};
  assert(!VisibleQuads(tiles, 1800, 1800, source, viewport, dst, &quads));
  puts("Tile planning, Morton bytes, neighbour halos and viewport coverage "
       "passed");
}
