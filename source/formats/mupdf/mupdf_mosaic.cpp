// SPDX-License-Identifier: AGPL-3.0-or-later
// Zoomed PDF tile mosaic: bounded cache of fixed 256x256 GPU tiles rendered
// straight from the display list (see fixed_layout_mosaic_utils.h).

#include "formats/mupdf/mupdf_view.h"

#include "formats/common/fixed_layout_mosaic_config.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "shared/fixed_layout_perf.h"
#include "ui/text.h"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
using namespace fixed_layout_mosaic;
using mupdf_mosaic::Mosaic;
using mupdf_mosaic::Tile;

const uint32_t kPassBudgetMs = 20;
const size_t kTileScratchBytes = (size_t)kTexture * kTexture * 3;

size_t BudgetBytes(bool new3ds) {
  return new3ds ? 6u * 1024u * 1024u : 3584u * 1024u; // 48 / 28 full tiles
}

float ScaleFor(const Book::MuPdfState *s) {
  return ComputeFitScale(s->page_width, s->page_height, s->target_top_width,
                         s->target_top_height) *
         ComputeEffectiveMuPdfZoom(s->document_kind, s->viewport.zoom_index);
}

// Same extra samples as the classic interactive tile on the wide top screen.
void DensityFor(const Book::MuPdfState *s, float *dx, float *dy) {
  *dx = s->wide_top_screen && s->target_top_width == 400 ? 2.0f : 1.0f;
  *dy = s->wide_top_screen && s->target_top_height == 400 ? 2.0f : 1.0f;
}

void EvictAt(Mosaic &m, size_t index) {
  m.bytes -= m.tiles[index].bytes;
  m.tiles.erase(m.tiles.begin() + index);
}

const Tile *Find(const Mosaic &m, int epoch, int col, int row) {
  for (size_t i = 0; i < m.tiles.size(); ++i)
    if (m.tiles[i].epoch == epoch && m.tiles[i].col == col &&
        m.tiles[i].row == row)
      return &m.tiles[i];
  return NULL;
}

int Missing(const Mosaic &m, const Range &r) {
  int n = 0;
  for (int row = r.r0; row < r.r1; ++row)
    for (int col = r.c0; col < r.c1; ++col)
      if (!Find(m, m.epoch, col, row))
        ++n;
  return n;
}

int NewestStaleEpoch(const Mosaic &m) {
  int best = -1;
  for (size_t i = 0; i < m.tiles.size(); ++i)
    if (m.tiles[i].epoch != m.epoch && m.tiles[i].epoch > best)
      best = m.tiles[i].epoch;
  return best;
}

// Do the tiles of the newest superseded epoch span the whole viewport?
bool StaleCovers(const Mosaic &m, const pdf_view_utils::NormalizedRect &vp) {
  const int epoch = NewestStaleEpoch(m);
  if (epoch < 0)
    return false;
  const Tile *any = NULL;
  for (size_t i = 0; i < m.tiles.size() && !any; ++i)
    if (m.tiles[i].epoch == epoch)
      any = &m.tiles[i];
  Grid g;
  if (!MakeGrid(any->page_w_px, any->page_h_px, &g))
    return false;
  const Range r = TilesFor(g, vp.left * any->page_w_px, vp.top * any->page_h_px,
                           (vp.left + vp.width) * any->page_w_px,
                           (vp.top + vp.height) * any->page_h_px, 0);
  if (r.Empty())
    return false;
  for (int row = r.r0; row < r.r1; ++row)
    for (int col = r.c0; col < r.c1; ++col)
      if (!Find(m, epoch, col, row))
        return false;
  return true;
}

void DropStale(Mosaic &m) {
  for (size_t i = m.tiles.size(); i > 0; --i)
    if (m.tiles[i - 1].epoch != m.epoch)
      EvictAt(m, i - 1);
}

bool EvictOne(Mosaic &m, const Range &keep, float cxt, float cyt) {
  std::vector<Entry> entries;
  for (size_t i = 0; i < m.tiles.size(); ++i) {
    const Entry e = {m.tiles[i].col, m.tiles[i].row, m.tiles[i].epoch,
                     m.tiles[i].stamp, m.tiles[i].bytes};
    entries.push_back(e);
  }
  const int victim = PickVictim(entries, m.epoch, keep, cxt, cyt);
  if (victim < 0)
    return false;
  EvictAt(m, (size_t)victim);
  return true;
}

bool UpdateGeometry(Book::MuPdfState *s, int page) {
  Mosaic &m = s->mosaic;
  if (m.page != page) {
    m.Clear();
    m.page = page;
  }
  const float scale = ScaleFor(s);
  float dx, dy;
  DensityFor(s, &dx, &dy);
  if (m.dx != dx || m.dy != dy) {
    // Wide toggled: tiles at the other density are useless.
    m.tiles.clear();
    m.bytes = 0;
  }
  if (m.zoom != s->viewport.zoom_index || std::fabs(m.scale - scale) > 1e-6f ||
      m.page_w != s->page_width || m.page_h != s->page_height || m.dx != dx ||
      m.dy != dy) {
    Grid g;
    if (!MakeGrid(s->page_width * scale * dx, s->page_height * scale * dy, &g))
      return false;
    ++m.epoch;
    m.zoom = s->viewport.zoom_index;
    m.scale = scale;
    m.dx = dx;
    m.dy = dy;
    m.page_w = s->page_width;
    m.page_h = s->page_height;
    m.grid = g;
  }
  return true;
}

bool EnsureList(Book::MuPdfState *s, int page, fz_display_list **out) {
  if (!EnsureMuPdfDisplayListForPage(s, page, out))
    return false;
  if (*out)
    return true;
  fz_display_list *list = NULL;
  if (!BuildMuPdfDisplayList(s->ctx, s->doc, page, &list, NULL)) {
    // Out of heap: the kept previous-page list is the first thing to give up.
    if (s->kept_display_list) {
      fz_drop_display_list(s->ctx, s->kept_display_list);
      s->kept_display_list = NULL;
      s->kept_display_list_page = -1;
      s->kept_display_list_bytes = 0;
      BuildMuPdfDisplayList(s->ctx, s->doc, page, &list, NULL);
    }
  }
  if (!list)
    return false;
  s->cached_display_list = list;
  s->cached_display_list_page = page;
  s->cached_display_list_bytes = LastMuPdfDisplayListBytes();
  *out = list;
  return true;
}

// Renders one tile into `scratch` and uploads it. `blocked`: nothing could be
// evicted to make room (not a hard failure). Returns true when inserted.
bool RenderTile(Book::MuPdfState *s, Text *ts, int col, int row,
                fz_display_list *list, const fz_matrix &ctm, void *scratch,
                const Range &keep, float cxt, float cyt, bool *blocked,
                bool *hard_fail) {
  Mosaic &m = s->mosaic;
  const PixelRect in = Interior(m.grid, col, row);
  const int tw = PaddedDim(in.w), th = PaddedDim(in.h);
  const size_t bytes = (size_t)tw * th * 2;
  const size_t budget = BudgetBytes(s->is_new_3ds);
  while (m.bytes + bytes > budget)
    if (!EvictOne(m, keep, cxt, cyt)) {
      *blocked = true;
      return false;
    }
  fixed_layout_gpu::TileTexture texture;
  for (int attempt = 0; !texture.Create(ts, tw, th); ++attempt) {
    if (attempt >= 4 || !EvictOne(m, keep, cxt, cyt)) {
      // Nothing left to give back: if the mosaic is empty the GPU is unusable.
      if (m.tiles.empty())
        *hard_fail = true;
      else
        *blocked = true;
      return false;
    }
  }
  // The texture covers interior plus the one-pixel halo (and power-of-two pad).
  const fz_irect bbox = fz_make_irect(in.x - 1, in.y - 1, in.x - 1 + tw,
                                      in.y - 1 + th);
  const bool hw = kHardwareUpload;
  const uint64_t started = fixed_perf::Now();
  if (!RenderMuPdfTileToBuffer(s->ctx, list, ctm, bbox, hw,
                               (unsigned char *)scratch)) {
    *hard_fail = true;
    return false;
  }
  fixed_perf::Record(s->doc, m.page, m.zoom, "pdf.tile_raster",
                     fixed_perf::Now() - started, 1, (size_t)tw * th * 3, tw,
                     th);
  bool uploaded;
  if (hw) {
    uploaded = texture.UploadHardware(scratch, tw, th);
  } else {
    const raster_image::Rgb8View view = {(const unsigned char *)scratch,
                                         (size_t)tw * th * 3, tw, th, tw * 3, 3};
    uploaded = texture.UploadRgb8(view);
  }
  if (!uploaded) {
    *hard_fail = true;
    return false;
  }
  m.tiles.emplace_back();
  Tile &t = m.tiles.back();
  t.col = col;
  t.row = row;
  t.epoch = m.epoch;
  t.stamp = ++m.clock;
  t.interior = in;
  t.tex_w = tw;
  t.tex_h = th;
  t.page_w_px = m.page_w * m.scale * m.dx;
  t.page_h_px = m.page_h * m.scale * m.dy;
  t.bytes = bytes;
  t.texture = std::move(texture);
  m.bytes += bytes;
  return true;
}

// The classic interactive tile (kept across a switch into mosaic zoom) can
// stand in for missing tiles while it still spans the viewport.
bool ClassicTileUsable(const Book::MuPdfState *s, int page,
                       const pdf_view_utils::NormalizedRect &vp) {
  const Book::MuPdfState::BitmapCache &c = s->current_interactive_tile;
  return MuPdfInteractiveTileCoversViewport(s, page, vp) &&
         (c.gpu.Matches(c.generation) || (c.PixelData() && c.PixelCount()));
}

void Update(Book::MuPdfState *s, Text *ts, int page,
            const pdf_view_utils::NormalizedRect &vp) {
  Mosaic &m = s->mosaic;
  const bool idle = m.idle_go;
  m.idle_go = false;
  if (!UpdateGeometry(s, page)) {
    m.failed = true;
    return;
  }
  const float wf = m.page_w * m.scale * m.dx, hf = m.page_h * m.scale * m.dy;
  const float vx0 = vp.left * wf, vy0 = vp.top * hf;
  const float vx1 = (vp.left + vp.width) * wf, vy1 = (vp.top + vp.height) * hf;
  const Range inner = TilesFor(m.grid, vx0, vy0, vx1, vy1, 0);
  const Range ring = TilesFor(m.grid, vx0, vy0, vx1, vy1, 1);
  for (size_t i = 0; i < m.tiles.size(); ++i)
    if (m.tiles[i].epoch == m.epoch &&
        ring.Contains(m.tiles[i].col, m.tiles[i].row))
      m.tiles[i].stamp = ++m.clock;
  const int visible_missing = Missing(m, inner);
  const int ring_missing = Missing(m, ring);
  const bool classic_cov = ClassicTileUsable(s, page, vp);
  const bool stale_cov = StaleCovers(m, vp) || classic_cov;
  // Zoom changed and the old tiles still span the view: keep presenting them
  // scaled until the idle pump releases the sharp tiles.
  const bool defer = s->refine_pending && !s->refine_go && visible_missing > 0 &&
                     stale_cov;
  const bool want_ring = idle && ring_missing > 0;
  bool attempted = false;
  int rendered = 0;
  if (!defer && (visible_missing > 0 || want_ring)) {
    // One unbuffered marker per page and zoom: PERF events are queued and
    // only flushed by the main loop, so they vanish if a draw never returns.
    if (m.traced_page != page || m.traced_epoch != m.epoch) {
      m.traced_page = page;
      m.traced_epoch = m.epoch;
      DBG_LOGF(m.reporter,
               "MOSAIC page=%d zoom=%d visible_missing=%d ring_missing=%d tiles=%u bytes=%u hw=%d dense=%.0fx%.0f",
               page, m.zoom, visible_missing, ring_missing,
               (unsigned)m.tiles.size(), (unsigned)m.bytes,
               kHardwareUpload ? 1 : 0, m.dx, m.dy);
    }
    fz_display_list *list = NULL;
    void *scratch = NULL;
    bool ok = EnsureList(s, page, &list) && list;
    if (ok) {
      scratch = fixed_layout_gpu::AllocLinearScratch(kTileScratchBytes);
      ok = scratch != NULL;
    }
    if (!ok) {
      m.failed = true;
    } else {
      const fz_rect bounds = fz_bound_display_list(s->ctx, list);
      fz_matrix ctm = MakeMuPdfRenderMatrix(bounds, m.scale);
      if (m.dx != 1.0f || m.dy != 1.0f)
        ctm = fz_concat(ctm, fz_scale(m.dx, m.dy));
      std::vector<Coord> order;
      OrderByDistance(m.grid, inner, want_ring ? ring : inner,
                      (vx0 + vx1) * 0.5f, (vy0 + vy1) * 0.5f, &order);
      // Even with nothing underneath (page turn: overview deferred, no stale
      // tiles) a pass stops at its budget once one tile exists: the frame
      // presents what is ready and the idle pump fetches the rest next frame,
      // so the page appears tile by tile instead of after all of them.
      const uint64_t pass_started = fixed_perf::Now();
      const u64 start_ms = osGetTime();
      for (size_t i = 0; i < order.size(); ++i) {
        const Coord c = order[i];
        if (Find(m, m.epoch, c.col, c.row))
          continue;
        const bool is_visible = inner.Contains(c.col, c.row);
        if (rendered > 0 && osGetTime() - start_ms >= kPassBudgetMs)
          break;
        attempted = true;
        bool blocked = false, hard = false;
        if (!RenderTile(s, ts, c.col, c.row, list, ctm, scratch,
                        is_visible ? inner : ring, (vx0 + vx1) * 0.5f / kInterior,
                        (vy0 + vy1) * 0.5f / kInterior, &blocked, &hard)) {
          if (hard)
            m.failed = true;
          break;
        }
        ++rendered;
      }
      fixed_perf::Record(s->doc, page, m.zoom, "pdf.tile_pass",
                         fixed_perf::Now() - pass_started, rendered ? 1 : 0,
                         (size_t)rendered, visible_missing, ring_missing);
    }
    fixed_layout_gpu::FreeLinearScratch(scratch);
  }
  if (m.failed)
    return;
  const int visible_left = Missing(m, inner);
  const int ring_left = Missing(m, ring);
  if (visible_left == 0) {
    DropStale(m);
    // Sharp tiles now span the view: the stand-in raster can go.
    if (s->current_interactive_tile.page >= 0)
      ResetBitmapCache(&s->current_interactive_tile);
    s->refine_pending = false;
    s->refine_go = false;
  }
  m.pending = visible_left > 0 || ring_left > 0;
  // A pass that tried and produced nothing is stuck (budget, memory): do not
  // keep the idle pump spinning on it.
  m.visible_pending = visible_left > 0;
  if (attempted && rendered == 0)
    m.pending = m.visible_pending = false;
}

void BuildDraws(const Mosaic &m, const pdf_view_utils::NormalizedRect &vp,
                const pdf_view_utils::PreviewLayout &dest, bool include_stale,
                std::vector<fixed_layout_gpu::TileDraw> *out) {
  std::vector<const Tile *> order;
  for (size_t i = 0; i < m.tiles.size(); ++i)
    if (include_stale || m.tiles[i].epoch == m.epoch)
      order.push_back(&m.tiles[i]);
  // Oldest (blurriest) first so sharper tiles draw over them.
  std::stable_sort(order.begin(), order.end(),
                   [](const Tile *a, const Tile *b) { return a->epoch < b->epoch; });
  for (size_t i = 0; i < order.size(); ++i) {
    const Tile &t = *order[i];
    Quad q;
    if (!MakeQuad(t.interior, t.tex_w, t.tex_h, vp.left * t.page_w_px,
                  vp.top * t.page_h_px, (vp.left + vp.width) * t.page_w_px,
                  (vp.top + vp.height) * t.page_h_px, (float)dest.x,
                  (float)dest.y, (float)dest.width, (float)dest.height, &q))
      continue;
    const fixed_layout_gpu::TileDraw d = {&t.texture, t.interior.w, t.interior.h,
                                          q.x, q.y, q.w, q.h,
                                          q.u0, q.v0, q.u1, q.v1};
    out->push_back(d);
  }
}
} // namespace

bool UseMuPdfMosaic(const Book::MuPdfState *s) {
  if (!fixed_layout_mosaic::kEnabled || !s || !s->ctx || !s->doc ||
      !debug_runtime::ForceSynchronousMuPdfRender() ||
      debug_runtime::DisableFixedLayoutGpuRenderer() ||
      s->gpu_draw_failed || s->mosaic.failed)
    return false;
  if (pdf_view_utils::ZoomForIndex(s->viewport.zoom_index) <= 2.0f)
    return false;
  const float scale = ScaleFor(s);
  float dx, dy;
  DensityFor(s, &dx, &dy);
  const double full_pixels = std::ceil(s->page_width * scale * dx + 1.0f) *
                             std::ceil(s->page_height * scale * dy + 1.0f);
  // New3DS keeps a page that fits its 4M-sample budget as one resident raster.
  if (s->is_new_3ds && full_pixels <= 4.0 * 1024.0 * 1024.0)
    return false;
  Grid g;
  return MakeGrid(s->page_width * scale * dx, s->page_height * scale * dy, &g);
}

bool MuPdfMosaicWantsIdleWork(const Book::MuPdfState *s) {
  return s && s->mosaic.pending && UseMuPdfMosaic(s);
}

bool DrawMuPdfMosaic(Book::MuPdfState *s, Text *ts, int page,
                     const pdf_view_utils::NormalizedRect &viewport,
                     const pdf_view_utils::PreviewLayout &top_layout) {
  Mosaic &m = s->mosaic;
  Update(s, ts, page, viewport);
  if (m.failed)
    return false;
  const fixed_layout_tiles::Rect vp = {viewport.left, viewport.top,
                                       viewport.width, viewport.height};
  const fixed_layout_tiles::Rect dst = {(float)top_layout.x, (float)top_layout.y,
                                        (float)top_layout.width,
                                        (float)top_layout.height};
  // Underlay: the low-resolution overview, for any area without a tile yet.
  bool underlay = false;
  Book::MuPdfState::BitmapCache &classic = s->current_interactive_tile;
  Book::MuPdfState::BitmapCache &preview = s->current_preview;
  const Range inner = TilesFor(
      m.grid, viewport.left * m.page_w * m.scale * m.dx,
      viewport.top * m.page_h * m.scale * m.dy,
      (viewport.left + viewport.width) * m.page_w * m.scale * m.dx,
      (viewport.top + viewport.height) * m.page_h * m.scale * m.dy, 0);
  const bool complete = Missing(m, inner) == 0;
  if (!complete && ClassicTileUsable(s, page, viewport)) {
    if (!classic.gpu.Matches(classic.generation))
      classic.gpu.Upload(ts, classic.PixelData(), classic.PixelCount(),
                         classic.bitmap_width, classic.bitmap_height,
                         classic.bitmap_width, classic.generation);
    underlay = classic.gpu.Matches(classic.generation) &&
               fixed_layout_gpu::Queue(
                   ts, classic.gpu,
                   {classic.left, classic.top, classic.width, classic.height},
                   vp, dst);
  }
  if (!underlay && !complete && BitmapCacheValid(preview, page) &&
      preview.PixelData()) {
    underlay = preview.gpu.Upload(ts, preview.PixelData(), preview.PixelCount(),
                                  preview.bitmap_width, preview.bitmap_height,
                                  preview.bitmap_width, preview.generation) &&
               fixed_layout_gpu::Queue(
                   ts, preview.gpu,
                   {preview.left, preview.top, preview.width, preview.height},
                   vp, dst);
  }
  std::vector<fixed_layout_gpu::TileDraw> draws;
  BuildDraws(m, viewport, top_layout, !complete, &draws);
  if (draws.empty())
    return underlay;
  return fixed_layout_gpu::QueueTiles(ts, &m, draws.data(), draws.size(),
                                      underlay);
}
