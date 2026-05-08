#pragma once

#include "book/book.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "formats/common/fixed_layout_preview_constants.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "formats/common/pdf_view_utils.h"

extern "C" {
#include "mupdf/fitz/color.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/device.h"
#include "mupdf/fitz/display-list.h"
#include "mupdf/fitz/document.h"
#include "mupdf/fitz/geometry.h"
#include "mupdf/fitz/outline.h"
#include "mupdf/fitz/pixmap.h"
#include "mupdf/fitz/util.h"
}

struct App;
extern fz_locks_context g_mupdf_locks_ctx;
extern u16 g_gray_to_rgb565[256];

static const int kPdfPreviewScreenWidth =
    fixed_layout_screen::kBottomScreenWidth;
static const int kPdfPreviewScreenHeight =
    fixed_layout_screen::kBottomScreenHeight;
static const int kPdfPreviewPadding = fixed_layout_preview::kPadding;
static const int kPdfZoomScreenWidth = fixed_layout_screen::kTopScreenWidth;
static const int kPdfZoomScreenHeight = fixed_layout_screen::kTopScreenHeight;
static const int kMuPdfAaLevel = 8;
static const float kPdfInteractiveScale = 1.0f;
static const u32 kPdfInteractiveDeferredDelayMs = 180;
static const u32 kPdfFinalDeferredDelayMs = 2200;
static const u32 kPdfPrefetchDeferredDelayMs = 3500;
static const int kPdfStripsOld3DS = 8;
static const int kPdfStripsNew3DS = 8;
static const u16 kPdfPaper = fixed_layout_preview::kPaper;
static const u16 kPdfFrame = fixed_layout_preview::kFrame;
static const u16 kPdfAccent = fixed_layout_preview::kViewportAccent;

struct RenderedMuPdfBitmap {
  int width;
  int height;
  std::vector<u16> pixels;

  RenderedMuPdfBitmap() : width(0), height(0), pixels() {}
};

struct MuPdfBitmapBoundsPx {
  int left;
  int top;
  int width;
  int height;

  MuPdfBitmapBoundsPx() : left(0), top(0), width(0), height(0) {}
};

struct MuPdfNavigationBounds {
  float left;
  float top;
  float width;
  float height;

  MuPdfNavigationBounds() : left(0.0f), top(0.0f), width(1.0f), height(1.0f) {}
};

enum class MuPdfDeferredStage {
  None = 0,
  Preview,
  Interactive,
  Final,
  Prefetch,
};

void InitMuPdfLocks();
void EnsureGrayLut();
bool DetectNew3ds();
u16 RGB565FromRgb8(unsigned char r, unsigned char g, unsigned char b);
void RGB565ToRgb8(u16 pixel, int *r, int *g, int *b);
bool IsMostlyWhite(u16 pixel);
float ComputeFitScale(float page_width, float page_height, int target_width,
                      int target_height);
float ComputeEffectiveMuPdfZoom(app_flow_utils::MuPdfDocumentKind kind,
                                int zoom_index);

inline void ResetBitmapCache(Book::MuPdfState::BitmapCache *cache) {
  if (!cache)
    return;
  cache->page = -1;
  cache->zoom_index = -1;
  cache->left = 0.0f;
  cache->top = 0.0f;
  cache->width = 1.0f;
  cache->height = 1.0f;
  cache->bitmap_width = 0;
  cache->bitmap_height = 0;
  cache->pixels.clear();
}

inline void ResetAdjacentSlot(Book::MuPdfState::AdjacentSlot *slot,
                              fz_context *ctx) {
  if (!slot)
    return;
  if (slot->display_list && ctx)
    fz_drop_display_list(ctx, slot->display_list);
  slot->page = -1;
  slot->display_list = NULL;
  ResetBitmapCache(&slot->preview);
  ResetBitmapCache(&slot->interactive_tile);
}

inline void StoreBitmapCache(Book::MuPdfState::BitmapCache *cache, int page,
                             int zoom_index, float left, float top,
                             float width, float height,
                             RenderedMuPdfBitmap *rendered) {
  if (!cache || !rendered)
    return;
  cache->page = page;
  cache->zoom_index = zoom_index;
  cache->left = left;
  cache->top = top;
  cache->width = width;
  cache->height = height;
  cache->bitmap_width = rendered->width;
  cache->bitmap_height = rendered->height;
  cache->pixels.swap(rendered->pixels);
}

inline bool BitmapCacheValid(const Book::MuPdfState::BitmapCache &cache,
                             int page) {
  return cache.page == page && cache.bitmap_width > 0 &&
         cache.bitmap_height > 0 && !cache.pixels.empty();
}
