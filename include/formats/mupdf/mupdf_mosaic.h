#pragma once
// Zoomed-PDF tile mosaic state: fixed 256x256 GPU textures covering the page at
// one zoom ("epoch"), bounded LRU, drawn under the sharp tiles of the current
// epoch. See fixed_layout_mosaic_utils.h for the geometry.
#include "formats/common/fixed_layout_gpu_renderer.h"
#include "formats/common/fixed_layout_mosaic_utils.h"

#include <cstdint>
#include <vector>

class IStatusReporter;

namespace mupdf_mosaic {
struct Tile {
  int col = 0, row = 0, epoch = 0;
  uint32_t stamp = 0;
  fixed_layout_mosaic::PixelRect interior = {0, 0, 0, 0};
  int tex_w = 0, tex_h = 0;
  float page_w_px = 1, page_h_px = 1; // exact page size at this tile's scale
  size_t bytes = 0;
  fixed_layout_gpu::TileTexture texture;
};

struct Mosaic {
  int page = -1, zoom = -1, epoch = 0;
  float scale = 0, page_w = 0, page_h = 0;
  // Wide (800px) top screen: extra samples along each axis, as the classic
  // interactive tile renders them. Grid and tiles live in these dense pixels.
  float dx = 1, dy = 1;
  fixed_layout_mosaic::Grid grid = {0, 0, 0, 0};
  std::vector<Tile> tiles;
  size_t bytes = 0;
  uint32_t clock = 0;
  // Visible or margin tiles are still missing: keeps the idle pump running.
  bool pending = false;
  // Part of the *visible* area is still missing (sharp tiles outrank the
  // overview render in the idle pump).
  bool visible_pending = false;
  // Set by the idle pump: the next draw may also render margin tiles.
  bool idle_go = false;
  // A tile could not be produced at all (no linear scratch, raster error):
  // the classic interactive tile takes over for the rest of the session.
  bool failed = false;
  // Debug log sink for unbuffered progress markers (hardware hang triage).
  IStatusReporter *reporter = nullptr;
  int traced_page = -1, traced_epoch = -1;

  void Clear() {
    tiles.clear();
    bytes = 0;
    pending = false;
    visible_pending = false;
    idle_go = false;
    page = -1;
    zoom = -1;
  }
  bool HasTiles() const { return !tiles.empty(); }
};
} // namespace mupdf_mosaic
