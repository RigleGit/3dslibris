#include "formats/common/fixed_layout_gpu_renderer.h"
#include "formats/common/fixed_layout_gpu_upload.h"
#include "shared/fixed_layout_perf.h"
#include "ui/text.h"
#include <memory>
#include <string>
#include <cstdio>
GpuBoundary gpu;
enum class AppMode { Browser, Book };
struct App {
  struct {
    bool IsSuspended() const {
      return false;
    }
  } lifecycle_state_;
  struct {
    AppMode mode = AppMode::Book;
  } nav_;
  std::unique_ptr<Text> ts{new Text};
  bool PresentIfDirty();
};
#include "gpu_present_under_test.inc"
using namespace fixed_layout_gpu;
int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "reset";
  App app;
  if (mode == "decoded_retry") {
    std::vector<unsigned char> samples(2048u * 2048u * 3u, 90);
    DecodedUpload upload(app.ts.get(), 7, 85, 115);
    const auto sink = upload.Sink();
    const raster_image::Rgb8View large = {
        samples.data(), samples.size(), 2048, 2048, 2048*3, 3};
    assert(!sink.consume(sink.user, large, 240, 400));
    // Preview aspect follows the document, not native/Wide pixel density.
    assert(upload.preview_width == 69 && upload.preview_height == 115);
    assert(upload.preview[0] == ((11 << 11) | (22 << 5) | 11));
    // A raster retry can reduce coverage/density within the same logical
    // generation. A failed large upload must not block that smaller attempt.
    upload.preview_box_width = upload.preview_box_height = 0;
    const raster_image::Rgb8View small = {
        samples.data(), samples.size(), 8, 8, 8*3, 3};
    assert(sink.consume(sink.user, small, 240, 400));
    assert(upload.texture.Matches(7));
    upload.texture.Reset();
    Shutdown(app.ts.get());
    assert(gpu.live_textures == 0 && gpu.linear_blocks.empty());
    puts("GPU smaller-raster upload retry and document-aspect preview passed");
    return 0;
  }
  {
    TextureCache cache;
    const int width = mode == "failure" ? 1200 : 960;
    std::vector<u16> pixels((size_t)width * 1000);
    for (size_t i = 0; i < pixels.size(); ++i)
      pixels[i] = (u16)(i * 1739 + 27);
    if (mode == "failure")
      gpu.fail_after = 1;
    if (mode == "low_memory")
      gpu.linear_free = 1024u * 1024u + 64u * 1024u;
    if (mode == "fragmented")
      gpu.max_linear_block = 64u * 1024u;
    std::vector<unsigned char> rgb;
    if (mode == "rgb_upload") {
      rgb.resize((size_t)width * 1000 * 3);
      for (size_t i = 0; i < pixels.size(); ++i) {
        rgb[3*i] = (pixels[i] >> 11) << 3;
        rgb[3*i+1] = ((pixels[i] >> 5) & 63) << 2;
        rgb[3*i+2] = (pixels[i] & 31) << 3;
      }
    }
    const bool ok = mode == "rgb_upload"
        ? cache.UploadRgb(app.ts.get(), {rgb.data(), rgb.size(), width, 1000, width*3, 3}, 1)
        : cache.Upload(app.ts.get(), pixels.data(), pixels.size(), width, 1000, width, 1);
    if (mode == "failure") {
      assert(!ok && gpu.live_textures == 0 && gpu.linear_blocks.empty());
    } else {
      assert(ok);
      if (mode == "upload" || mode == "rgb_upload" || mode == "low_memory" || mode == "fragmented") {
        const size_t limit = (mode == "upload" || mode == "rgb_upload") ? 8 : 32;
        assert(gpu.transfers.size() <= limit &&
               "bounded staging must group adjacent transfers");
        if (mode == "low_memory" || mode == "fragmented")
          assert(gpu.transfers.size() == 32);
        const size_t calls = gpu.transfers.size();
        assert(cache.Upload(app.ts.get(), nullptr, 0, 960, 1000, 960, 1));
        assert(gpu.transfers.size() == calls);
        const auto &allocation = *gpu.textures.begin();
        const u16 *tiled = (const u16 *)allocation.first;
        for (unsigned y : {1u, 500u, 1000u})
          for (unsigned x : {1u, 500u, 960u})
            assert(tiled[fixed_layout_tiles::MortonOffset(x, y, 1024)] ==
                   pixels[(y - 1) * 960 + x - 1]);
      } else {
        assert(Queue(app.ts.get(), cache, {0, 0, 1, 1}, {0, 0, 1, 1},
                     {0, 0, 240, 400}));
        if (mode == "rotation") {
          for(int orientation=0;orientation<3;++orientation) for(bool wide:{false,true}) for(int turn=0;turn<4;++turn) {
            app.ts->orientation=orientation;gpu.wide=wide;
            const int w=orientation==2 ? 400 : 240,h=orientation==2 ? 240 : 400;
            assert(Queue(app.ts.get(),cache,{0,0,1,1},{0,0,1,1},
                         {0,0,(float)(turn%2 ? h : w),(float)(turn%2 ? w : h)},turn));
            const size_t transfers=gpu.transfers.size();gpu.draw_views.clear();
            assert(Present(app.ts.get()));assert(gpu.draw_views.size()>1);
            const auto bg=gpu.draw_views.front(),m=gpu.draw_views.back();
            // Source origin moves to the appropriate screen corner; background
            // origin stays fixed. Both then share the global reader transform.
            const int x[]={0,w,w,0},y[]={0,0,h,h};
            assert(m.x==bg.a*x[turn]+bg.c*y[turn]+bg.x);
            assert(m.y==bg.b*x[turn]+bg.d*y[turn]+bg.y);
            const int a[]={1,0,-1,0},b[]={0,1,0,-1};
            assert(m.a==bg.a*a[turn]+bg.c*b[turn]);
            assert(m.b==bg.b*a[turn]+bg.d*b[turn]);
            assert(gpu.transfers.size()==transfers);
          }
        } else if (mode == "resize") {
          assert(Present(app.ts.get()));
          // The source stays resident; the stale background must make room
          // before the larger target is allocated.
          gpu.target_max_textures = 1;
          gpu.wide = true;
          assert(Queue(app.ts.get(), cache, {0, 0, 1, 1}, {0, 0, 1, 1},
                       {0, 0, 240, 400}));
          assert(gpu.restores == 0 && "replacing a full GPU target needs no CPU readback");
          assert(Present(app.ts.get()));
        } else if (mode == "reset") {
          const int frames = gpu.frames;
          cache.Reset();
          assert(gpu.restores == 0 && gpu.frames == frames &&
                 "discarding a pending source must not render/read back its "
                 "old frame");
          assert(!Active(app.ts.get()));
        } else {
          app.ts->dirty_bottom = true;
          const int before = gpu.full_flushes + gpu.framebuffer_flushes;
          if (mode == "frame_failure")
            gpu.fail_frame = true;
          assert(app.PresentIfDirty());
          assert(gpu.full_flushes + gpu.framebuffer_flushes == before + 1 &&
                 "each frame needs one coherent flush, including a failed "
                 "FrameBegin");
          gpu.fail_frame = false;
          if (mode == "flush") {
            // Both backbuffers must still receive the retained top, and a
            // bottom-only frame needs its CPU flush even without a GPU draw.
            app.ts->dirty_bottom = true;
            const int frames = gpu.frames;
            const int flushes = gpu.framebuffer_flushes;
            assert(app.PresentIfDirty());
            assert(gpu.frames == frames &&
                   gpu.framebuffer_flushes == flushes + 1);
            const int restores = gpu.restores;
            cache.Reset();
            assert(gpu.restores == restores && Active(app.ts.get()));
            Software(app.ts.get());
            assert(gpu.restores == restores + 1 && !Active(app.ts.get()));
            // A cancelled first GPU frame after software mode must not revive
            // the old target/framebuffer as though it were still current.
            assert(cache.Upload(app.ts.get(), pixels.data(), pixels.size(), 960,
                                1000, 960, 2));
            assert(Queue(app.ts.get(), cache, {0, 0, 1, 1}, {0, 0, 1, 1},
                         {0, 0, 240, 400}));
            cache.Reset();
            assert(!Active(app.ts.get()) && gpu.restores == restores + 1);
          }
        }
      }
    }
  }
  Shutdown(app.ts.get());
  assert(gpu.live_textures == 0 && gpu.linear_blocks.empty());
  std::printf(
      "GPU source lifetime, presentation and upload boundary: %s passed\n",
      mode.c_str());
}
