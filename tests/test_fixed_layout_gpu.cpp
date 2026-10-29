#include "formats/common/fixed_layout_gpu_renderer.h"
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
    const bool ok = cache.Upload(app.ts.get(), pixels.data(), pixels.size(),
                                 width, 1000, width, 1);
    if (mode == "failure") {
      assert(!ok && gpu.live_textures == 0 && gpu.linear_blocks.empty());
    } else {
      assert(ok);
      if (mode == "upload" || mode == "low_memory" || mode == "fragmented") {
        const size_t limit = mode == "upload" ? 8 : 32;
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
        if (mode == "resize") {
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
