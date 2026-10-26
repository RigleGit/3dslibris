#include "formats/mupdf/mupdf_render.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <cmath>
#include <algorithm>
#include "shared/debug_runtime_mode.h"
#include "shared/debug_log.h"

u16 g_gray_to_rgb565[256];
void EnsureGrayLut() {}
static bool reject_native_pixels = false;
void *operator new[](size_t bytes, const std::nothrow_t &) noexcept {
  if (reject_native_pixels) { reject_native_pixels = false; return nullptr; }
  return ::operator new[](bytes);
}

#include "viewport_owner.inc"
// No worker is started in the runtime policy under test. Fail if the owner
// unexpectedly reaches a platform worker operation.
void ShutdownMuPdfWorker(Book::MuPdfState *) { assert(false); }
void LightEvent_Clear(LightEvent *) { assert(false); }
void CancelMuPdfIncrementalRenderState(Book::MuPdfState *);
bool PromoteMuPdfAdjacentSlotIfMatching(Book::MuPdfState *, int);
#include "interactive_owner.inc"

static void WritePdf(const char *path, const std::string &stream, int width, int height) {
  const std::string objects[] = {
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(width) + " " + std::to_string(height) + "] /Resources << >> /Contents 4 0 R >>",
    "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream"
  };
  FILE *file = fopen(path, "wb"); assert(file);
  fprintf(file, "%%PDF-1.4\n");
  long offsets[4];
  for (int i = 0; i < 4; ++i) {
    offsets[i] = ftell(file);
    fprintf(file, "%d 0 obj\n%s\nendobj\n", i + 1, objects[i].c_str());
  }
  const long xref = ftell(file);
  fprintf(file, "xref\n0 5\n0000000000 65535 f \n");
  for (long offset : offsets) fprintf(file, "%010ld 00000 n \n", offset);
  fprintf(file, "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
  assert(fclose(file) == 0);
}

static void TestLandscapePageVisibleTile(const char *path) {
  // Two separated 60x60 squares on an apaisado page. Each must stay square
  // after pixel-aspect compensation, and panning must reveal the other one.
  WritePdf(path, "1 0 0 rg 70 90 60 60 re f\n0 0 1 rg 270 90 60 60 re f\n", 400, 240);
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT); assert(ctx);
  fz_register_document_handlers(ctx);
  fz_document *doc = fz_open_document(ctx, path); assert(doc);
  for (bool landscape : {false, true}) {
    Book::MuPdfState state;
    state.ctx = ctx; state.doc = doc;
    state.page_width = 400; state.page_height = 240;
    state.document_kind = app_flow_utils::MuPdfDocumentKind::Pdf;
    state.target_top_width = landscape ? 400 : 240;
    state.target_top_height = landscape ? 240 : 400;
    state.wide_top_screen = true;
    state.viewport.zoom_index = 2;
    state.viewport.center_x = 0.25f; state.viewport.center_y = 0.5f;
    assert(EnsureCurrentMuPdfInteractiveTile(&state, 0));
    auto &cache = state.current_interactive_tile;
    assert(std::abs(cache.bitmap_width - (landscape ? 800 : 240)) <= 2);
    assert(std::abs(cache.bitmap_height - (landscape ? 240 : 800)) <= 2);
    assert(cache.width < 1 && cache.height < 1);
    fz_display_list *interpreted = state.cached_display_list;
    assert(interpreted);
    // The portrait viewport fully contains the red square and excludes blue.
    if (!landscape) {
      int x0 = cache.bitmap_width, x1 = -1, y0 = cache.bitmap_height, y1 = -1;
      for (int y = 0; y < cache.bitmap_height; ++y) {
        for (int x = 0; x < cache.bitmap_width; ++x) {
          const u16 pixel = cache.PixelData()[y * cache.bitmap_width + x];
          assert(pixel != 0x001F);
          if (pixel == 0xF800) { x0 = std::min(x0,x); x1 = std::max(x1,x); y0 = std::min(y0,y); y1 = std::max(y1,y); }
        }
      }
      assert(x1 >= x0 && y1 >= y0);
      assert(std::abs((x1-x0+1) * 2 - (y1-y0+1)) <= 2);
      state.viewport.center_x = 0.75f;
      assert(EnsureCurrentMuPdfInteractiveTile(&state, 0));
      bool blue = false;
      for (size_t i = 0; i < cache.PixelCount(); ++i) {
        assert(cache.PixelData()[i] != 0xF800);
        blue |= cache.PixelData()[i] == 0x001F;
      }
      assert(blue);
      assert(state.cached_display_list == interpreted);
    }
    // Every zoom remains a bounded visible tile, instead of allocating the
    // full page and dropping native density at higher zoom presets.
    for (int zoom : {3,4,5,6}) {
      state.viewport.zoom_index = zoom;
      assert(EnsureCurrentMuPdfInteractiveTile(&state, 0));
      assert(std::abs(cache.bitmap_width - (landscape ? 800 : 240)) <= 2);
      assert(std::abs(cache.bitmap_height - (landscape ? 240 : 800)) <= 2);
      assert(cache.PixelCount() <= 802u * 242u);
    }
    ResetBitmapCache(&cache);
    state.viewport.zoom_index = 2;
    reject_native_pixels = true;
    assert(EnsureCurrentMuPdfInteractiveTile(&state, 0));
    assert(!cache.native_pixels && cache.width < 1 && cache.height < 1);
    assert(std::abs(cache.bitmap_width - state.target_top_width) <= 2);
    assert(std::abs(cache.bitmap_height - state.target_top_height) <= 2);
    assert(state.cached_display_list == interpreted);
    // The non-wide path retains the existing full-page raster policy.
    ResetBitmapCache(&cache);
    state.wide_top_screen = false;
    assert(EnsureCurrentMuPdfInteractiveTile(&state, 0));
    assert(cache.width == 1 && cache.height == 1);
    assert(cache.bitmap_width == (landscape ? 600 : 360));
    assert(cache.bitmap_height == (landscape ? 360 : 216));
    if (state.cached_display_list) fz_drop_display_list(ctx,state.cached_display_list);
  }
  fz_drop_document(ctx,doc); fz_drop_context(ctx);
}

struct ControlledAllocation {
  bool reject = false;
  size_t attempts = 0;
};
static void *ControlledMalloc(void *user, size_t size) {
  auto *control = static_cast<ControlledAllocation *>(user);
  ++control->attempts;
  return control->reject ? nullptr : std::malloc(size);
}
static void *ControlledRealloc(void *user, void *ptr, size_t size) {
  auto *control = static_cast<ControlledAllocation *>(user);
  ++control->attempts;
  return control->reject ? nullptr : std::realloc(ptr, size);
}
static void ControlledFree(void *, void *ptr) { std::free(ptr); }
static void TestFailedTileKeepsRequestIdentity(const char *path) {
  ControlledAllocation control;
  fz_alloc_context allocator{&control,ControlledMalloc,ControlledRealloc,ControlledFree};
  fz_context *ctx = fz_new_context(&allocator,NULL,FZ_STORE_DEFAULT); assert(ctx);
  fz_register_document_handlers(ctx);
  fz_document *doc = fz_open_document(ctx,path); assert(doc);
  Book::MuPdfState state; state.ctx=ctx; state.doc=doc;
  state.page_width=400; state.page_height=240; state.wide_top_screen=true;
  state.document_kind=app_flow_utils::MuPdfDocumentKind::Pdf;
  state.viewport.zoom_index=2; state.viewport.center_x=0.25f;
  assert(EnsureCurrentMuPdfInteractiveTile(&state,0));
  state.viewport.center_x=0.75f; control.reject=true;
  assert(!EnsureCurrentMuPdfInteractiveTile(&state,0));
  auto &cache=state.current_interactive_tile;
  // Draw resets unmatched request keys. A failed tile must remain identified
  // while being invalid as a bitmap, or its failure guard is lost next frame.
  assert(cache.page == 0 && cache.zoom_index == 2);
  assert(!BitmapCacheValid(cache,0));
  size_t attempts=control.attempts;
  assert(!EnsureCurrentMuPdfInteractiveTile(&state,0));
  assert(control.attempts == attempts);
  control.reject=false; state.viewport.center_x=0.5f;
  assert(EnsureCurrentMuPdfInteractiveTile(&state,0));
  assert(BitmapCacheValid(cache,0));
  if(state.cached_display_list) fz_drop_display_list(ctx,state.cached_display_list);
  fz_drop_document(ctx,doc); fz_drop_context(ctx);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  std::string stripes = "0 g\n";
  for (int y = 0; y < 400; ++y) stripes += "0 " + std::to_string(y) + " 240 0.5 re f\n";
  WritePdf(argv[1], stripes, 240, 400);
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT); assert(ctx);
  fz_register_document_handlers(ctx);
  fz_document *doc = fz_open_document(ctx, argv[1]); assert(doc);
  float width = 0, height = 0;
  RenderedMuPdfBitmap normal, native;
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &normal, &width, &height));
  assert(normal.width == 240 && normal.height == 400 && width == 240 && height == 400);
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &native, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(native.width == 240 && native.height == 800 && native.native_pixels);
  for (int y = 0; y < 800; ++y)
    assert(native.PixelData()[y * 240 + 120] == (y & 1 ? 0 : 0xFFFF));
  // Transfer ownership to the real cache without another allocation/copy.
  Book::MuPdfState::BitmapCache cache;
  const u16 *address = native.PixelData();
  StoreBitmapCache(&cache, 0, 0, 0, 0, 1, 1, &native);
  assert(cache.PixelData() == address && !native.native_pixels);
  assert(BitmapCacheValid(cache, 0) && cache.PixelCount() == 240u * 800u);
  ResetBitmapCache(&cache); assert(!BitmapCacheValid(cache, 0));
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &native, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 2, 1));
  assert(native.width == 480 && native.height == 400);
  RenderedMuPdfBitmap capped;
  assert(RenderMuPdfBitmap(ctx, doc, 0, 3, &capped, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(capped.width == 720 && capped.height == 1200);
  // An optional allocation failure is handled, leaving the normal renderer
  // usable and the real MuPDF context clean enough for repeated teardown.
  reject_native_pixels = true;
  RenderedMuPdfBitmap failed;
  assert(!RenderMuPdfBitmap(ctx, doc, 0, 1, &failed, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(failed.width == 0 && failed.PixelCount() == 0);
  assert(fz_caught(ctx) == FZ_ERROR_NONE);
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &failed, &width, &height));
  assert(failed.width == 240 && failed.height == 400);
  fz_drop_document(ctx, doc); fz_drop_context(ctx);
  TestLandscapePageVisibleTile(argv[1]);
  TestFailedTileKeepsRequestIdentity(argv[1]);
  puts("PASS: real PDF native detail, bounded horizontal-page tiles, proportions, pan and allocation recovery");
}
