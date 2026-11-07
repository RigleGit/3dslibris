#include "formats/common/fixed_layout_mosaic_config.h"
#include "formats/common/fixed_layout_mosaic_utils.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace fixed_layout_mosaic;

static void TestPadding() {
  assert(PaddedDim(1) == 8 && PaddedDim(6) == 8 && PaddedDim(7) == 16);
  assert(PaddedDim(14) == 16 && PaddedDim(15) == 32);
  assert(PaddedDim(kInterior) == kTexture && PaddedDim(kInterior + 1) == 512);
  for (int n = 1; n <= 300; ++n) {
    const int p = PaddedDim(n);
    assert(p >= 8 && (p & (p - 1)) == 0 && p >= n + 2 && (p == 8 || p / 2 < n + 2));
  }
}

static void TestGrid() {
  Grid g;
  assert(!MakeGrid(0, 10, &g) && !MakeGrid(10, -1, &g) && !MakeGrid(NAN, 10, &g));
  assert(!MakeGrid(100000, 100, &g));
  assert(MakeGrid(254.0f, 254.0f, &g) && g.cols == 1 && g.rows == 1);
  assert(MakeGrid(254.5f, 508.0f, &g) && g.cols == 2 && g.rows == 2 && g.width == 255);
  assert(MakeGrid(960.2f, 1245.0f, &g) && g.width == 961 && g.cols == 4 && g.rows == 5);
  // Interiors tile the page exactly, edge tiles are short.
  long covered = 0;
  for (int r = 0; r < g.rows; ++r)
    for (int c = 0; c < g.cols; ++c) {
      const PixelRect p = Interior(g, c, r);
      assert(p.x == c * kInterior && p.y == r * kInterior && p.w > 0 && p.h > 0);
      assert(p.w <= kInterior && p.h <= kInterior);
      covered += (long)p.w * p.h;
    }
  assert(covered == (long)g.width * g.height);
  assert(Interior(g, 3, 0).w == 961 - 3 * 254);
}

static void TestRanges() {
  Grid g;
  assert(MakeGrid(960, 1245, &g));
  Range r = TilesFor(g, 0, 0, 240, 400, 0); // 240x400 screen at the origin
  assert(r.c0 == 0 && r.c1 == 1 && r.r0 == 0 && r.r1 == 2 && r.Count() == 2);
  r = TilesFor(g, 250, 250, 260, 260, 0); // straddles a corner of four tiles
  assert(r.c0 == 0 && r.c1 == 2 && r.r0 == 0 && r.r1 == 2);
  r = TilesFor(g, 254, 254, 300, 300, 0); // starts exactly on a border
  assert(r.c0 == 1 && r.c1 == 2 && r.r0 == 1 && r.r1 == 2);
  r = TilesFor(g, 300, 300, 400, 400, 1); // margin ring, clamped at the grid
  assert(r.c0 == 0 && r.c1 == 3 && r.r0 == 0 && r.r1 == 3);
  r = TilesFor(g, 900, 1200, 2000, 3000, 1); // clamped at the far edge
  assert(r.c1 == g.cols && r.r1 == g.rows);
  assert(TilesFor(g, -50, -50, -10, -10, 0).Empty());
  assert(TilesFor(g, 100, 100, 100, 200, 0).Empty());
  assert(TilesFor(g, 5000, 0, 6000, 10, 0).Empty());
  // A panning viewport only newly needs the tiles that enter it.
  const Range a = TilesFor(g, 0, 0, 400, 240, 0), b = TilesFor(g, 20, 0, 420, 240, 0);
  int fresh = 0;
  for (int rr = b.r0; rr < b.r1; ++rr)
    for (int c = b.c0; c < b.c1; ++c)
      if (!a.Contains(c, rr))
        ++fresh;
  assert(fresh == 0 && "20px pan inside the same tiles needs no raster");
  const Range c2 = TilesFor(g, 120, 0, 520, 240, 0);
  fresh = 0;
  for (int rr = c2.r0; rr < c2.r1; ++rr)
    for (int c = c2.c0; c < c2.c1; ++c)
      if (!a.Contains(c, rr))
        ++fresh;
  assert(fresh == 1 && "crossing one border adds exactly one column of one tile");
}

static void TestOrder() {
  Grid g;
  assert(MakeGrid(960, 1245, &g));
  const Range inner = TilesFor(g, 200, 200, 600, 440, 0), outer = TilesFor(g, 200, 200, 600, 440, 1);
  std::vector<Coord> order;
  OrderByDistance(g, inner, outer, 400, 320, &order);
  assert((int)order.size() == outer.Count());
  for (int i = 0; i < inner.Count(); ++i)
    assert(inner.Contains(order[i].col, order[i].row) && "visible tiles first");
  for (size_t i = inner.Count(); i < order.size(); ++i)
    assert(!inner.Contains(order[i].col, order[i].row));
  // Nearest visible tile leads.
  const PixelRect first = Interior(g, order[0].col, order[0].row);
  assert(first.x <= 400 && 400 < first.x + first.w);
}

static void TestQuads() {
  Grid g;
  assert(MakeGrid(960, 1245, &g));
  const float vx0 = 100.3f, vy0 = 50.7f, vx1 = 500.3f, vy1 = 290.7f; // 400x240
  const float dx = 0, dy = 0, dw = 400, dh = 240;
  std::vector<Quad> quads;
  std::vector<PixelRect> rects;
  const Range r = TilesFor(g, vx0, vy0, vx1, vy1, 0);
  for (int row = r.r0; row < r.r1; ++row)
    for (int col = r.c0; col < r.c1; ++col) {
      const PixelRect p = Interior(g, col, row);
      Quad q;
      assert(MakeQuad(p, PaddedDim(p.w), PaddedDim(p.h), vx0, vy0, vx1, vy1, dx, dy, dw, dh, &q));
      quads.push_back(q);
      rects.push_back(p);
    }
  // Union covers exactly the destination, neighbours share bit-identical edges.
  float area = 0;
  for (size_t i = 0; i < quads.size(); ++i) {
    area += quads[i].w * quads[i].h;
    assert(quads[i].x >= dx && quads[i].y >= dy);
    assert(quads[i].x + quads[i].w <= dx + dw + 1e-3f);
    for (size_t j = 0; j < quads.size(); ++j) {
      if (rects[j].x == rects[i].x + rects[i].w && rects[j].y == rects[i].y)
        assert(quads[j].x == quads[i].x + quads[i].w && "no seam between columns");
      if (rects[j].y == rects[i].y + rects[i].h && rects[j].x == rects[i].x)
        assert(quads[j].y == quads[i].y + quads[i].h && "no seam between rows");
    }
  }
  assert(std::fabs(area - dw * dh) < 0.5f);
  // 1:1 mapping samples exactly one texel per pixel, halo included.
  Quad q;
  const PixelRect p = Interior(g, 1, 1); // x=254..508
  assert(MakeQuad(p, 256, 256, 254, 254, 508, 508, 0, 0, 254, 254, &q));
  assert(q.x == 0 && q.w == 254);
  assert(std::fabs(q.u0 - 1.0f / 256) < 1e-6f && std::fabs(q.u1 - 255.0f / 256) < 1e-6f);
  assert(std::fabs(q.v0 - (1 - 1.0f / 256)) < 1e-6f && std::fabs(q.v1 - (1 - 255.0f / 256)) < 1e-6f);
  // Viewport cutting into a tile moves only that tile's UVs.
  assert(MakeQuad(p, 256, 256, 300, 254, 508, 508, 0, 0, 208, 254, &q));
  assert(std::fabs(q.u0 - (300 - 254 + 1) / 256.0f) < 1e-6f && q.x == 0);
  // Outside the viewport, or degenerate.
  assert(!MakeQuad(p, 256, 256, 0, 0, 254, 254, 0, 0, 254, 254, &q));
  assert(!MakeQuad(p, 256, 256, 300, 300, 300, 400, 0, 0, 1, 1, &q));
  assert(!MakeQuad(p, 0, 256, 254, 254, 508, 508, 0, 0, 254, 254, &q));
}

static Entry E(int c, int r, int epoch, uint32_t stamp) {
  Entry e = {c, r, epoch, stamp, 131072};
  return e;
}

static void TestLru() {
  const Range keep = {1, 1, 3, 3};
  std::vector<Entry> e;
  assert(PickVictim(e, 5, keep, 2, 2) < 0);
  e.push_back(E(1, 1, 5, 10));
  e.push_back(E(2, 2, 5, 11));
  assert(PickVictim(e, 5, keep, 2, 2) < 0 && "visible tiles are never evicted");
  e.push_back(E(0, 0, 5, 3));
  e.push_back(E(6, 6, 5, 4));
  e.push_back(E(2, 0, 5, 5));
  const int far = PickVictim(e, 5, keep, 2, 2);
  assert(far == 3 && "farthest current tile goes first");
  // Superseded epochs go before any current tile, oldest epoch first.
  e.push_back(E(1, 1, 4, 100));
  e.push_back(E(1, 2, 3, 200));
  assert(PickVictim(e, 5, keep, 2, 2) == 6);
  e.erase(e.begin() + 6);
  assert(PickVictim(e, 5, keep, 2, 2) == 5);
  // Equal distance: older use goes first.
  std::vector<Entry> t;
  t.push_back(E(0, 5, 1, 9));
  t.push_back(E(5, 0, 1, 2));
  assert(PickVictim(t, 1, keep, 0, 0) == 0 || PickVictim(t, 1, keep, 0, 0) == 1);
  t[0] = E(4, 0, 1, 9);
  t[1] = E(0, 4, 1, 2);
  assert(PickVictim(t, 1, keep, 2, 2) == 1);
  // Bounded growth: evicting until the new tile fits never touches `keep`.
  std::vector<Entry> cache;
  const size_t budget = 28 * 131072;
  size_t bytes = 0;
  uint32_t clock = 0;
  for (int i = 0; i < 200; ++i) {
    const int c = i % 9, r = (i / 9) % 9;
    const Range view = {c, r, c + 2, r + 2};
    for (int dr = 0; dr < 2; ++dr)
      for (int dc = 0; dc < 2; ++dc) {
        bool present = false;
        for (size_t k = 0; k < cache.size(); ++k)
          if (cache[k].col == c + dc && cache[k].row == r + dr)
            present = true;
        if (present)
          continue;
        while (bytes + 131072 > budget) {
          const int v = PickVictim(cache, 0, view, c + 1, r + 1);
          assert(v >= 0);
          assert(!view.Contains(cache[v].col, cache[v].row));
          bytes -= cache[v].bytes;
          cache.erase(cache.begin() + v);
        }
        cache.push_back(E(c + dc, r + dr, 0, ++clock));
        bytes += 131072;
        assert(bytes <= budget);
      }
  }
}

static void TestKeepPolicy() {
  assert(!ShouldKeepDisplayList(10u << 20, 0, true) && "unknown size is never kept");
  assert(!ShouldKeepDisplayList(470760, 200000, false) && "a 2DS XL heap cannot afford it");
  assert(!ShouldKeepDisplayList(233640, 50000, false));
  assert(ShouldKeepDisplayList(2u << 20, 200000, false));
  assert(!ShouldKeepDisplayList(1u << 20, 400000, false));
  assert(ShouldKeepDisplayList(8u << 20, 1u << 20, true));
  assert(!ShouldKeepDisplayList(3u << 20, 1u << 20, true));
}

static void TestSwitches() {
  static_assert(sizeof(char[kEnabled || !kEnabled ? 1 : 0]) == 1, "switches are constexpr");
  (void)kHardwareUpload; (void)kDeferPreview; (void)kKeepPreviousList;
}

int main() {
  TestPadding();
  TestGrid();
  TestRanges();
  TestOrder();
  TestQuads();
  TestLru();
  TestKeepPolicy();
  TestSwitches();
  puts("fixed-layout mosaic utils passed");
  return 0;
}
