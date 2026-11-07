#include "formats/common/fixed_layout_gpu_upload.h"
#include "formats/common/fixed_layout_mosaic_config.h"
#include "formats/common/fixed_layout_mosaic_utils.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/pdf_view_utils.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "shared/fixed_layout_perf.h"
#include "ui/text.h"
typedef unsigned char u8;
struct fz_context {};
struct fz_document {};
struct fz_display_list { int page; };
struct IStatusReporter {};
struct RenderedMuPdfBitmap { int width = 10, height = 10; std::vector<u16> pixels; };
#define DBG_LOGF_CAT(...) ((void)0)
#define DBG_LOGF(...) ((void)0)
namespace app_flow_utils { enum class MuPdfDocumentKind { Pdf }; }
namespace mupdf_render_policy_utils {
  static bool skip = false;
  bool ShouldSkipPdfPageRender(app_flow_utils::MuPdfDocumentKind, int, size_t) { return skip; }
}
struct Cache {
  int page = -1, zoom_index = -1, bitmap_width = 0, bitmap_height = 0;
  bool gpu_only = false;
  uint64_t generation = 0;
  fixed_layout_gpu::TextureCache gpu;
  float left = 0, top = 0, width = 1, height = 1;
  std::vector<u16> pixels;
  const u16 *PixelData() const { return pixels.data(); }
  size_t PixelCount() const { return pixels.size(); }
};
struct Book {
  struct MuPdfState {
    typedef Cache BitmapCache;
    struct { int zoom_index = 0, max_zoom_index = 6; bool interaction_active = false; } viewport;
    fz_context context;
    fz_document document;
    fz_context *ctx = &context;
    fz_document *doc = &document;
    IStatusReporter *reporter = nullptr;
    Cache current_preview, current_interactive_tile, current_final_zoom;
    struct { bool valid = false; } preview_panel;
    Cache prev_slot, next_slot;
    bool final_cache_pending = true;
    float page_width = 600, page_height = 900;
    int target_bottom_width = 240, target_bottom_height = 320;
    int target_top_width = 240, target_top_height = 400;
    bool is_new_3ds = true, wide_top_screen = false;
    bool gpu_draw_failed = false;
    bool refine_pending = false, refine_go = false;
    struct { bool has = false, cleared = false; void Clear() { has = false; cleared = true; }
             bool HasTiles() const { return has; } } mosaic;
    bool preview_pending = false;
    size_t cached_display_list_bytes = 0, kept_display_list_bytes = 0;
    fz_display_list *kept_display_list = nullptr;
    int kept_display_list_page = -1;
    int interactive_render_failed_page=-1, interactive_render_failed_zoom=-1;
    int final_render_failed_page=-1, final_render_failed_zoom=-1;
    app_flow_utils::MuPdfDocumentKind document_kind = app_flow_utils::MuPdfDocumentKind::Pdf;
    int page_too_complex_for_device = -1, cached_display_list_page = -1;
    std::vector<unsigned char> page_complexity_status = std::vector<unsigned char>(4, 0);
    fz_display_list *cached_display_list = nullptr;
  };
};
// Keep this display-list fixture on the independent preview path; it does
// not validate reuse of the full-page interactive raster.
namespace debug_runtime { bool sync=false; bool ForceSynchronousMuPdfRender() { return sync; } }
const int kPdfPreviewPadding = fixed_layout_preview::kPadding;
const float kPdfInteractiveScale=1.0f;
static pdf_view_utils::NormalizedRect requested_viewport={0,0,1,1};
pdf_view_utils::NormalizedRect ComputeCurrentMuPdfViewport(const Book::MuPdfState *) { return requested_viewport; }
bool EnsureCurrentMuPdfInteractiveTile(Book::MuPdfState *, int, Text * = nullptr);
bool EnsureCurrentMuPdfPreviewCache(Book::MuPdfState *, int, Text * = nullptr);
bool IsMuPdfPageRenderBlocked(Book::MuPdfState *, int);
bool HasMuPdfInteractiveRenderFailed(const Book::MuPdfState *,int) { return false; }
float ComputeFitScale(float w,float h,int tw,int th) { return std::min(tw/w,th/h); }
float ComputeEffectiveMuPdfZoom(app_flow_utils::MuPdfDocumentKind,int z) { return pdf_view_utils::ZoomForIndex(z); }
static bool mosaic_on = false;
bool UseMuPdfMosaic(const Book::MuPdfState *) { return mosaic_on; }
size_t LastMuPdfDisplayListBytes() { return 4096; }
static int renders = 0, builds = 0, drops = 0, cancels = 0, complexity_checks = 0;
static bool fail = false;
static std::vector<std::string> events;
bool PromoteMuPdfAdjacentSlotIfMatching(Book::MuPdfState *, int) { return false; }
void CancelMuPdfIncrementalRenderState(Book::MuPdfState *) { ++cancels; events.push_back("cancel"); }
void fz_drop_display_list(fz_context *, fz_display_list *p) { assert(!events.empty() && (events.back() == "cancel" || events.back() == "drop") && "each old list must be cancelled before disposal"); events.push_back("drop"); ++drops; delete p; }
bool BitmapCacheValid(const Cache &c, int p) { return c.page == p && c.bitmap_width>0 && c.bitmap_height>0 && (c.gpu_only || c.pixels.size()==(size_t)c.bitmap_width*c.bitmap_height); }
float ComputeMuPdfPreviewScale(float, float, int, int) { return 0.2f; }
bool EstimateMuPdfPageRenderComplexity(fz_context *, fz_document *, int, int *, size_t *) { ++complexity_checks; return true; }
bool RenderMuPdfBitmap(fz_context *, fz_document *, int p, float, RenderedMuPdfBitmap *bitmap, float *, float *, const void *, fz_display_list *reuse, fz_display_list **out, IStatusReporter *, const char * = nullptr, float=1, float=1, bool=false, size_t=0, const raster_image::Rgb8Sink * = nullptr) {
  ++renders; events.push_back("render");
  if (fail) return false;
  bitmap->pixels.assign(100,0x1234);
  if (reuse) assert(reuse->page == p);
  else if (out) { *out = new fz_display_list{p}; ++builds; }
  return true;
}
void ComputeBitmapContentBoundsNormalized(const RenderedMuPdfBitmap &, float *, float *, float *, float *) {}
void StoreBitmapCache(Cache *c, int p, int z, float l, float t, float w, float h, RenderedMuPdfBitmap *b) { c->page=p; c->zoom_index=z; c->left=l; c->top=t; c->width=w; c->height=h; c->bitmap_width=b->width; c->bitmap_height=b->height; c->pixels.swap(b->pixels); c->gpu_only=false; }
void ResetBitmapCache(Cache *c) { c->page = -1; c->pixels.clear(); c->gpu_only=false; }
void ResetAdjacentSlot(Cache *c, fz_context *) { c->page = -1; }
#include "mupdf_preview_under_test.inc"
static void TestCpuFallbackSkipsGpuOnlyRaster() {
  Text text;
  text.display.width=10; text.display.height=10;
  Cache cache;
  cache.page=0; cache.bitmap_width=cache.bitmap_height=10;
  cache.gpu_only=true;
  std::vector<u16> screen(100,0x7777);
  const pdf_view_utils::NormalizedRect viewport={0,0,1,1};
  for (bool region : {false,true}) {
    const bool drawn=region
        ? BlitBitmapCacheViewportRegion(&text,screen.data(),10,10,10,0,10,cache,viewport,false)
        : BlitBitmapCacheViewport(&text,screen.data(),10,10,10,cache,viewport,false);
    assert(!drawn && "GPU-only raster must let the CPU caller try its preview");
  }
  cache.gpu_only=false; cache.pixels.assign(100,0x1234);
  assert(BlitBitmapCacheViewport(&text,screen.data(),10,10,10,cache,viewport,false));
  for(u16 pixel : screen) assert(pixel==0x1234);
  std::fill(screen.begin(),screen.end(),0x7777);
  assert(BlitBitmapCacheViewportRegion(&text,screen.data(),10,10,10,2,5,cache,viewport,false));
  for(int y=0;y<10;++y) for(int x=0;x<10;++x)
    assert(screen[y*10+x]==(y>=2 && y<5?0x1234:0x7777));
}
static void TestZoomRasterReuse() {
  debug_runtime::sync=true;
  for (bool gpu_only : {false,true}) {
    Book::MuPdfState s;
    Cache &c=s.current_interactive_tile;
    c.page=0; c.zoom_index=6; c.bitmap_width=600; c.bitmap_height=900;
    c.gpu_only=gpu_only;
    if(!gpu_only) c.pixels.assign(600*900,0x1234);
    s.viewport.zoom_index=4;
    requested_viewport={0,0,1,1};
    const u16 *pixels=c.PixelData();
    const int before=renders;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(c.page==0 && "zooming out must preserve a covering high-resolution raster");
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0));
    assert(renders==before && c.PixelData()==pixels && c.gpu_only==gpu_only);
    // A cropped source cannot satisfy a wider viewport, even at high density.
    c.left=0.25f; c.top=0.25f; c.width=0.5f; c.height=0.5f;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(c.page==-1);
  }
  for (bool wide : {false,true}) {
    Book::MuPdfState s;
    Cache &c=s.current_interactive_tile;
    c.page=0; c.zoom_index=6; c.bitmap_width=240; c.bitmap_height=360;
    c.pixels.assign(240*360,0x1234); s.viewport.zoom_index=4;
    s.wide_top_screen=wide;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(c.page==0 && s.refine_pending==wide &&
           "wide density must count before reusing a normal-density raster; a covering tile is kept and refined");
  }
  {
    // Zoom in with a tile that still spans the viewport: present it scaled now
    // and rasterize the sharp tile only after the idle pump releases it.
    Book::MuPdfState s;
    Cache &c=s.current_interactive_tile;
    c.page=0; c.zoom_index=2; c.bitmap_width=120; c.bitmap_height=180;
    c.pixels.assign(120*180,0x1234); s.viewport.zoom_index=5;
    requested_viewport={0.3f,0.3f,0.2f,0.2f};
    const int before=renders;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(c.page==0 && s.refine_pending && !s.refine_go);
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && renders==before &&
           c.zoom_index==2 && s.refine_pending && "refine must wait for the pump");
    s.refine_go=true;
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && renders==before+1);
    assert(c.zoom_index==5 && !s.refine_pending && !s.refine_go);
    events.push_back("cancel"); fz_drop_display_list(s.ctx,s.cached_display_list);
    // A viewport that leaves the stale tile must not be deferred.
    Book::MuPdfState t;
    Cache &d=t.current_interactive_tile;
    d.page=0; d.zoom_index=2; d.bitmap_width=120; d.bitmap_height=180;
    d.left=0.0f; d.top=0.0f; d.width=0.3f; d.height=0.3f;
    d.pixels.assign(120*180,0x1234); t.viewport.zoom_index=5;
    requested_viewport={0.6f,0.6f,0.2f,0.2f};
    ResetMuPdfDeferredCachesForSynchronousRender(&t);
    assert(d.page==-1 && !t.refine_pending && "an uncovered viewport re-rasters immediately");
    requested_viewport={0,0,1,1};
  }
  {
    Book::MuPdfState s;
    Cache &c=s.current_interactive_tile;
    c.page=0; c.zoom_index=6; c.bitmap_width=120; c.bitmap_height=180;
    c.pixels.assign(120*180,0x1234); s.viewport.zoom_index=4;
    s.gpu_draw_failed=true;
    const int before=renders;
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0));
    assert(!s.gpu_draw_failed && "a replacement raster must get a fresh GPU attempt");
    assert(renders==before+1 && "insufficient density must reraster even when the old region covers the viewport");
    s.gpu_draw_failed=true;
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && renders==before+1);
    assert(s.gpu_draw_failed && "a resident CPU fallback must not retry GPU every pan");
    events.push_back("cancel"); fz_drop_display_list(s.ctx,s.cached_display_list);
  }
  {
    Book::MuPdfState s;
    Cache &c=s.current_interactive_tile;
    c.page=0; c.zoom_index=6; c.bitmap_width=600; c.bitmap_height=900;
    c.gpu_only=true; c.left=0.25f; c.top=0.25f; c.width=c.height=0.5f;
    s.viewport.zoom_index=4;
    requested_viewport={0.3f,0.3f,0.2f,0.2f};
    const int before=renders;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && renders==before);
    assert(c.gpu_only && c.left==0.25f);
  }
  requested_viewport={0,0,1,1};
  debug_runtime::sync=false;
}
static void TestMosaicAndKeptList() {
  debug_runtime::sync=true;
  {
    // Mosaic zoom: no classic raster is prepared, and a zoom change marks the
    // existing stand-in tiles for idle refinement.
    mosaic_on=true;
    Book::MuPdfState s;
    s.viewport.zoom_index=6;
    const int before=renders;
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && renders==before);
    assert(s.current_interactive_tile.page==-1);
    s.refine_pending=true;
    assert(EnsureCurrentMuPdfInteractiveTile(&s,0) && s.refine_pending &&
           "the mosaic owns the refine flags; the classic path must not clear them");
    s.mosaic.has=false;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(!s.refine_pending && "nothing to refine without stand-in tiles");
    s.mosaic.has=true;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(s.refine_pending && !s.refine_go);
    {
      // Switching into mosaic zoom keeps a classic tile that still spans the
      // viewport as a scaled stand-in; a tile that does not is released.
      Book::MuPdfState t;
      Cache &c=t.current_interactive_tile;
      c.page=0; c.zoom_index=4; c.bitmap_width=120; c.bitmap_height=180;
      c.pixels.assign(120*180,0x1234); t.viewport.zoom_index=6;
      requested_viewport={0.3f,0.3f,0.2f,0.2f};
      ResetMuPdfDeferredCachesForSynchronousRender(&t);
      assert(c.page==0 && t.refine_pending && !t.refine_go);
      c.left=0.0f; c.top=0.0f; c.width=0.25f; c.height=0.25f;
      ResetMuPdfDeferredCachesForSynchronousRender(&t);
      assert(c.page==-1 && !t.refine_pending);
      requested_viewport={0,0,1,1};
    }
    mosaic_on=false;
    s.mosaic.cleared=false;
    ResetMuPdfDeferredCachesForSynchronousRender(&s);
    assert(s.mosaic.cleared && "leaving the mosaic zoom frees its tiles");
  }
  {
    // Going back to the previous page reuses its kept display list.
    Book::MuPdfState s;
    s.cached_display_list=new fz_display_list{5};
    s.cached_display_list_page=5;
    s.kept_display_list=new fz_display_list{4};
    s.kept_display_list_page=4;
    s.kept_display_list_bytes=777;
    const int builds_before=builds;
    fz_display_list *out=nullptr;
    events.clear();
    assert(EnsureMuPdfDisplayListForPage(&s,4,&out));
    assert(out && out->page==4 && s.cached_display_list==out &&
           s.cached_display_list_page==4 && s.cached_display_list_bytes==777);
    assert(builds==builds_before && !s.kept_display_list &&
           "host policy has no heap figure, so the list being left is dropped");
    events.push_back("cancel"); fz_drop_display_list(s.ctx,s.cached_display_list);
    // An unrelated page drops the kept list instead of keeping two.
    Book::MuPdfState t;
    t.cached_display_list=new fz_display_list{5};
    t.cached_display_list_page=5;
    t.kept_display_list=new fz_display_list{3};
    t.kept_display_list_page=3;
    assert(EnsureMuPdfDisplayListForPage(&t,9,&out) && !out);
    assert(!t.kept_display_list && !t.cached_display_list);
  }
  {
    // The overview is deferred but the complexity gate is not.
    Book::MuPdfState s;
    mupdf_render_policy_utils::skip=true;
    s.page_complexity_status.assign(4,0);
    assert(IsMuPdfPageRenderBlocked(&s,1) && s.page_too_complex_for_device==1);
    mupdf_render_policy_utils::skip=false;
    s.page_complexity_status.assign(4,0);
    assert(!IsMuPdfPageRenderBlocked(&s,1));
  }
  debug_runtime::sync=false;
}
int main() {
  TestCpuFallbackSkipsGpuOnlyRaster();
  Book::MuPdfState s;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 0));
  assert(builds == 1 && s.cached_display_list && "preview must retain interpretation for the main view");
  fz_display_list *list = nullptr;
  assert(EnsureMuPdfDisplayListForPage(&s, 0, &list));
  assert(list == s.cached_display_list && list->page == 0);
  s.current_interactive_tile.page = 0;
  s.current_final_zoom.page = 0;
  ResetMuPdfDeferredCachesForSynchronousRender(&s);
  assert(s.cached_display_list == list && drops == 0 && "zoom must preserve page interpretation");
  assert(s.cached_display_list_page == 0 && s.current_preview.page == 0);
  assert(s.current_interactive_tile.page == -1 && s.current_final_zoom.page == -1);
  assert(cancels > 0 && !s.final_cache_pending);

  s.preview_panel.valid = true;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 0) && renders == 1);
  assert(s.preview_panel.valid && "unchanged preview must preserve the displayed panel");
  // Rebuilding just the bitmap (e.g. geometry) can reuse the same list.
  s.current_preview.page = -1;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 0) && builds == 1);
  assert(complexity_checks == 1 && "regenerating a preview must reuse the page's complexity result");
  assert(!s.preview_panel.valid && "regenerated pixels must invalidate the displayed panel");
  events.clear();
  assert(EnsureCurrentMuPdfPreviewCache(&s, 1) && builds == 2 && drops == 1);
  assert((events == std::vector<std::string>{"cancel", "drop", "render"}));
  fail = true;
  assert(!EnsureCurrentMuPdfPreviewCache(&s, 2));
  assert(!s.cached_display_list && drops == 2);
  assert(s.current_preview.page == 1 && s.cached_display_list_page == -1);
  fail = false;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 2));
  assert(builds == 3 && s.cached_display_list->page == 2 && s.current_preview.page == 2);
  const int before_skip=renders;
  mupdf_render_policy_utils::skip = true;
  assert(!EnsureCurrentMuPdfPreviewCache(&s, 3));
  assert(s.page_too_complex_for_device == 3 && builds == 3 && renders == before_skip);
  assert(s.current_preview.page == 2 && s.cached_display_list->page == 2);
  const int after_skip = complexity_checks;
  assert(!EnsureCurrentMuPdfPreviewCache(&s, 3));
  assert(complexity_checks == after_skip && "rejected pages must not reload streams on every retry");
  assert(!EnsureMuPdfDisplayListForPage(&s, 2, nullptr));
  assert(!EnsureMuPdfDisplayListForPage(nullptr, 2, &list));
  assert(!EnsureCurrentMuPdfPreviewCache(nullptr, 0));
  events.push_back("cancel"); fz_drop_display_list(s.ctx, s.cached_display_list);
  TestZoomRasterReuse();
  TestMosaicAndKeptList();
  puts("MuPDF display-list and cross-zoom raster reuse passed");
}
