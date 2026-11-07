#include "formats/common/fixed_layout_mosaic_utils.h"
#include <algorithm>
#include <cmath>

namespace fixed_layout_mosaic {
int PaddedDim(int interior) {
  int p = 8;
  while (p < interior + 2)
    p *= 2;
  return p;
}
bool MakeGrid(float w, float h, Grid *out) {
  if (!out || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 ||
      w > 16000.0f || h > 16000.0f)
    return false;
  const int W = (int)std::ceil(w), H = (int)std::ceil(h);
  out->width = W;
  out->height = H;
  out->cols = (W + kInterior - 1) / kInterior;
  out->rows = (H + kInterior - 1) / kInterior;
  return out->cols <= 64 && out->rows <= 64;
}
PixelRect Interior(const Grid &g, int col, int row) {
  PixelRect r;
  r.x = col * kInterior;
  r.y = row * kInterior;
  r.w = std::min(kInterior, g.width - r.x);
  r.h = std::min(kInterior, g.height - r.y);
  return r;
}
static void Axis(float a, float b, int extent, int count, int margin, int *i0,
                 int *i1) {
  const float lo = std::max(0.0f, a), hi = std::min((float)extent, b);
  if (!(hi > lo)) {
    *i0 = *i1 = 0;
    return;
  }
  int first = (int)std::floor(lo / kInterior);
  int last = (int)std::ceil(hi / kInterior); // exclusive
  first = std::max(0, first - margin);
  last = std::min(count, last + margin);
  *i0 = first;
  *i1 = std::max(first, last);
}
Range TilesFor(const Grid &g, float x0, float y0, float x1, float y1,
               int margin) {
  Range r = {0, 0, 0, 0};
  Axis(x0, x1, g.width, g.cols, margin, &r.c0, &r.c1);
  Axis(y0, y1, g.height, g.rows, margin, &r.r0, &r.r1);
  if (r.Empty())
    r.c0 = r.r0 = r.c1 = r.r1 = 0;
  return r;
}
namespace {
struct Ranked {
  float d;
  Coord c;
};
bool Less(const Ranked &a, const Ranked &b) {
  if (a.d != b.d)
    return a.d < b.d;
  if (a.c.row != b.c.row)
    return a.c.row < b.c.row;
  return a.c.col < b.c.col;
}
} // namespace
void OrderByDistance(const Grid &g, const Range &inner, const Range &outer,
                     float cx, float cy, std::vector<Coord> *out) {
  if (!out)
    return;
  out->clear();
  std::vector<Ranked> a, b;
  for (int r = outer.r0; r < outer.r1; ++r)
    for (int c = outer.c0; c < outer.c1; ++c) {
      const PixelRect p = Interior(g, c, r);
      const float dx = p.x + p.w * 0.5f - cx, dy = p.y + p.h * 0.5f - cy;
      Ranked item = {dx * dx + dy * dy, {c, r}};
      (inner.Contains(c, r) ? a : b).push_back(item);
    }
  std::sort(a.begin(), a.end(), Less);
  std::sort(b.begin(), b.end(), Less);
  for (size_t i = 0; i < a.size(); ++i)
    out->push_back(a[i].c);
  for (size_t i = 0; i < b.size(); ++i)
    out->push_back(b[i].c);
}
bool MakeQuad(const PixelRect &t, int tex_w, int tex_h, float vx0, float vy0,
              float vx1, float vy1, float dx, float dy, float dw, float dh,
              Quad *out) {
  if (!out || !(vx1 > vx0) || !(vy1 > vy0) || tex_w <= 0 || tex_h <= 0)
    return false;
  const float x0 = std::max(vx0, (float)t.x), y0 = std::max(vy0, (float)t.y);
  const float x1 = std::min(vx1, (float)(t.x + t.w));
  const float y1 = std::min(vy1, (float)(t.y + t.h));
  if (!(x1 > x0) || !(y1 > y0))
    return false;
  const float sx = dw / (vx1 - vx0), sy = dh / (vy1 - vy0);
  const float qx0 = dx + (x0 - vx0) * sx, qy0 = dy + (y0 - vy0) * sy;
  const float qx1 = dx + (x1 - vx0) * sx, qy1 = dy + (y1 - vy0) * sy;
  out->x = qx0;
  out->y = qy0;
  out->w = qx1 - qx0;
  out->h = qy1 - qy0;
  // Same Tex3DS convention as fixed_layout_tiles::VisibleQuads: row zero is v=1.
  out->u0 = (x0 - t.x + 1) / tex_w;
  out->v0 = 1 - (y0 - t.y + 1) / tex_h;
  out->u1 = (x1 - t.x + 1) / tex_w;
  out->v1 = 1 - (y1 - t.y + 1) / tex_h;
  return true;
}
int PickVictim(const std::vector<Entry> &e, int current, const Range &keep,
               float cx, float cy) {
  int best = -1;
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i].epoch == current)
      continue;
    if (best < 0 || e[i].epoch < e[best].epoch ||
        (e[i].epoch == e[best].epoch && e[i].stamp < e[best].stamp))
      best = (int)i;
  }
  if (best >= 0)
    return best;
  float far_d = -1;
  for (size_t i = 0; i < e.size(); ++i) {
    if (keep.Contains(e[i].col, e[i].row))
      continue;
    const float dx = e[i].col + 0.5f - cx, dy = e[i].row + 0.5f - cy;
    const float d = dx * dx + dy * dy;
    if (best < 0 || d > far_d || (d == far_d && e[i].stamp < e[best].stamp)) {
      best = (int)i;
      far_d = d;
    }
  }
  return best;
}
bool ShouldKeepDisplayList(size_t heap_free, size_t list_bytes,
                           bool is_new_3ds) {
  if (list_bytes == 0)
    return false;
  const size_t reserve = is_new_3ds ? 2u * 1024u * 1024u : 512u * 1024u;
  return heap_free >= list_bytes + list_bytes / 2 + reserve;
}
} // namespace fixed_layout_mosaic
