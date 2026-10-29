#include "formats/common/fixed_layout_tile_grid_utils.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/raster_image_source.h"
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
    // Halo samples a neighbour; unused complete 8x8 padding blocks are zero.
    for (int y = 0; y < t.tex_height; ++y)
      for (int x = 0; x < t.tex_width; ++x) {
        int sx =
            std::max(0, std::min(1022, t.x + std::min(x, t.width + 1) - 1));
        int sy = std::max(0, std::min(2, t.y + std::min(y, t.height + 1) - 1));
        const bool unused = (x & ~7) >= t.width + 2 ||
                            (y & ~7) >= t.height + 2;
        assert(out[MortonOffset(x, y, t.tex_width)] ==
               (unused ? 0 : image[sy * 1023 + sx]));
      }
  }
  // Unused complete padding blocks must not read image rows. They are never
  // sampled by visible quads, even with the one-pixel linear-filter halo.
  assert(BuildGrid(53, 21, &tiles, &bytes));
  std::vector<uint16_t> solid(53 * 21, 0x1234);
  std::vector<uint16_t> padded(64 * 32, 0xffff);
  assert(SwizzleChunk(tiles[0], solid.data(), solid.size(), 53, 53, 21, 0,
                      padded.size(), padded.data()));
  for (unsigned y = 0; y < 32; ++y)
    for (unsigned x = 0; x < 64; ++x)
      assert(padded[MortonOffset(x, y, 64)] ==
             (x >= 56 || y >= 24 ? 0 : 0x1234));
  // Fusing the pixmap conversion must preserve the old RGB565 tile bytes and
  // filtered preview exactly, including gray+alpha and padded source strides.
  for (int components : {1, 2, 3, 4}) {
    const int w = 53, h = 21, stride = w * components + 7;
    std::vector<unsigned char> rgb((size_t)stride * h, 0xcd);
    std::vector<uint16_t> converted(w * h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        unsigned char *p = rgb.data() + y * stride + x * components;
        p[0] = (x * 31 + y * 7) & 255;
        if (components >= 3) { p[1] = (y * 19) & 255; p[2] = (x * 3) & 255; }
        const unsigned r = p[0], g = components < 3 ? r : p[1],
                       b = components < 3 ? r : p[2];
        converted[y * w + x] = (r >> 3 << 11) | (g >> 2 << 5) | (b >> 3);
      }
    const raster_image::Rgb8View source = {
        rgb.data(), rgb.size(), w, h, stride, components};
    std::vector<uint16_t> tiled(64 * 32), expected(64 * 32);
    assert(SwizzleChunk(tiles[0], converted.data(), converted.size(), w, w, h,
                        0, expected.size(), expected.data()));
    for (size_t first = 0; first < tiled.size(); first += 73) {
      const size_t n = std::min((size_t)73, tiled.size() - first);
      assert(SwizzleRgb8Chunk(tiles[0], source, first, n, tiled.data() + first));
    }
    assert(tiled == expected);
    std::vector<uint16_t> preview(7 * 5), old_preview(7 * 5);
    fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(
        old_preview.data(), 7, 7, 5, 0, 0, 7, 5, converted.data(), converted.size(),
        w, h, 0, 0, w, h, true);
    assert(fixed_layout_blit_utils::ScaleRgb8Preview(source, 7, 5, preview.data()));
    assert(preview == old_preview);
    auto invalid = source;
    invalid.byte_count = 1;
    assert(!SwizzleRgb8Chunk(tiles[0], invalid, 0, 64, tiled.data()));
    invalid = source; invalid.stride = w * components - 1;
    assert(!fixed_layout_blit_utils::ScaleRgb8Preview(invalid, 7, 5, preview.data()));
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
