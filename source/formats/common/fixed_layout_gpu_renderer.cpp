#include "formats/common/fixed_layout_gpu_renderer.h"
#ifdef __3DS__
#include "ui/text.h"
#include "shared/debug_runtime_mode.h"
#include "shared/fixed_layout_perf.h"
#include <citro2d.h>
#include <algorithm>
#include <new>
#include <utility>

namespace fixed_layout_gpu {
struct TextureCache::Impl {
  std::vector<fixed_layout_tiles::Tile> plan;
  std::vector<C3D_Tex> textures;
  int width, height;
  Impl() : width(0), height(0) {}
  ~Impl() {
    for (size_t i = 0; i < textures.size(); ++i)
      C3D_TexDelete(&textures[i]);
  }
};

struct TileTexture::Impl {
  C3D_Tex tex;
  int width, height;
  bool vram;
};

struct Renderer {
  struct Item {
    C3D_Tex *tex;
    Tex3DS_SubTexture sub;
    float x, y, w, h;
    const void *owner;
  };
  Text *text;
  bool initialized, init_failed, active, pending;
  C3D_RenderTarget *target;
  int physical_height, orientation, quarter_turns, bg_color, bg_orientation;
  bool bg_wide;
  TextureCache background;
  std::vector<Item> items;
  framebuffer_blit_utils::PhysicalFramebufferSyncState sync;
  uint64_t generation;
  u8 *last_fb;
  Renderer()
      : text(nullptr), initialized(false), init_failed(false), active(false),
        pending(false), target(nullptr), physical_height(0), orientation(0), quarter_turns(0),
        bg_color(-1), bg_orientation(-1), bg_wide(false),
        generation(0), last_fb(nullptr) {}

  bool Ensure(Text *t) {
    if (!t || debug_runtime::DisableFixedLayoutGpuRenderer() || init_failed)
      return false;
    if (text && text != t)
      return false;
    text = t;
    if (!initialized) {
      if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        init_failed = true;
        return false;
      }
      if (!C2D_Init(64)) {
        C3D_Fini();
        init_failed = true;
        return false;
      }
      C2D_Prepare();
      initialized = true;
    }
    const int height = t->IsWideTopScreenActive() ? 800 : 400;
    if (!target || height != physical_height) {
      ToSoftware(t, true);
      // This background will be replaced before the next draw. Release it
      // before reserving the larger contiguous Wide render target in VRAM.
      background.Reset();
      if (target)
        C3D_RenderTargetDelete(target);
      target =
          C3D_RenderTargetCreate(240, height, GPU_RB_RGB8, C3D_DEPTHTYPE(-1));
      physical_height = height;
      sync = framebuffer_blit_utils::PhysicalFramebufferSyncState();
      last_fb = nullptr;
    }
    return target != nullptr;
  }
  // Background = the software top-screen buffer, drawn under every frame.
  bool PrepareBackground(Text *t) {
    const int color = t->GetColorMode(), orient = t->GetOrientation();
    const bool wide = t->IsWideTopScreenActive();
    if (!background.Valid() || color != bg_color || orient != bg_orientation ||
        wide != bg_wide) {
      const u16 *saved = t->GetScreen();
      t->SetScreen(t->screenleft);
      t->ClearScreen();
      const bool uploaded = background.Upload(
          t, t->screenleft,
          (size_t)t->BufferStride() * t->LogicalHeightFor(true),
          t->LogicalWidthFor(true), t->LogicalHeightFor(true),
          t->BufferStride(), ++generation);
      t->SetScreen(const_cast<u16 *>(saved));
      if (!uploaded)
        return false;
      bg_color = color;
      bg_orientation = orient;
      bg_wide = wide;
    }
    orientation = orient;
    return true;
  }
  void Begin(Text *t, int turns) {
    quarter_turns = turns & 3;
    active = true;
    pending = true;
    t->screenleft_dirty = false;
    t->screenleft_dirty_rect.valid = false;
  }
  bool Enqueue(Text *t, TextureCache &cache,
               const fixed_layout_tiles::Rect &src,
               const fixed_layout_tiles::Rect &vp,
               const fixed_layout_tiles::Rect &dst, int turns) {
    if (!cache.Valid() || !Ensure(t))
      return false;
    std::vector<fixed_layout_tiles::Quad> quads;
    if (!fixed_layout_tiles::VisibleQuads(cache.impl_->plan, cache.impl_->width,
                                          cache.impl_->height, src, vp, dst,
                                          &quads))
      return false;
    if (!PrepareBackground(t))
      return false;
    items.clear();
    for (size_t i = 0; i < quads.size(); ++i) {
      const fixed_layout_tiles::Quad &q = quads[i];
      const fixed_layout_tiles::Tile &tile = cache.impl_->plan[q.tile];
      Item item = {&cache.impl_->textures[q.tile],
                   {(u16)tile.width, (u16)tile.height, q.u0, q.v0, q.u1, q.v1},
                   q.x, q.y, q.width, q.height, &cache};
      items.push_back(item);
    }
    Begin(t, turns);
    return true;
  }
  bool EnqueueTiles(Text *t, const void *owner, const TileDraw *draws,
                    size_t count, bool append) {
    if (!draws || (!count && !append) || !Ensure(t) || !PrepareBackground(t))
      return false;
    if (append && !(active && pending))
      append = false;
    if (!append)
      items.clear();
    for (size_t i = 0; i < count; ++i) {
      const TileDraw &d = draws[i];
      if (!d.texture || !d.texture->Valid())
        continue;
      Item item = {&d.texture->impl_->tex,
                   {(u16)d.interior_width, (u16)d.interior_height, d.u0, d.v0,
                    d.u1, d.v1},
                   d.x, d.y, d.width, d.height, owner};
      items.push_back(item);
    }
    if (items.empty())
      return false;
    Begin(t, 0);
    return true;
  }
  // Withdraw a not yet presented frame that references a freed texture.
  void Drop(const void *owner, const C3D_Tex *tex) {
    if (!pending)
      return;
    bool hit = false;
    for (size_t i = 0; i < items.size() && !hit; ++i)
      hit = (owner && items[i].owner == owner) || items[i].tex == tex;
    if (!hit)
      return;
    items.clear();
    pending = false;
    // The completed target owns its pixels independently of the source. Keep
    // it available for bottom-only swaps and later partial software overlays.
    // An unpresented first frame has no retained image to preserve.
    if (!last_fb)
      ToSoftware(text, true);
  }
  void ViewMatrix() {
    // Unlinked targets do not get citro2d's automatic screen tilt. Use logical
    // panel coordinates explicitly; the wide viewport supplies double samples.
    C2D_SceneSize(240, physical_height, true);
    C2D_ViewReset();
    if (!orientation_utils::IsLandscape(orientation)) {
      if (orientation_utils::IsTurnedRight(orientation)) {
        C2D_ViewTranslate(0, 240);
        C2D_ViewRotateDegrees(-90);
      } else {
        C2D_ViewTranslate(400, 0);
        C2D_ViewRotateDegrees(90);
      }
    }
  }
  bool Draw() {
    if (!pending)
      return true;
    if (items.empty() || !C3D_FrameBegin(0))
      return false;
    C2D_TargetClear(target, C2D_Color32(255, 255, 255, 255));
    C3D_FrameDrawOn(target);
    ViewMatrix();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    bool ok = true;
    // A logical screen fits one background texture, including its halo.
    const fixed_layout_tiles::Tile &b = background.impl_->plan[0];
    Tex3DS_SubTexture sub = {(u16)b.width,
                             (u16)b.height,
                             1.0f / b.tex_width,
                             1.0f - 1.0f / b.tex_height,
                             (float)(b.width + 1) / b.tex_width,
                             1.0f - (float)(b.height + 1) / b.tex_height};
    C2D_Image image = {&background.impl_->textures[0], &sub};
    ok = C2D_DrawImageAt(image, 0, 0, 0);
    // Background stays in screen coordinates. Rotate only the source quads.
    const bool landscape=orientation_utils::IsLandscape(orientation);
    const int width=landscape ? 400 : 240, height=landscape ? 240 : 400;
    if(quarter_turns==1) {C2D_ViewTranslate(width,0);C2D_ViewRotateDegrees(90);}
    if(quarter_turns==2) {C2D_ViewTranslate(width,height);C2D_ViewRotateDegrees(180);}
    if(quarter_turns==3) {C2D_ViewTranslate(0,height);C2D_ViewRotateDegrees(-90);}
    for (size_t i = 0; i < items.size(); ++i) {
      Item &it = items[i];
      C2D_Image img = {&it.tex[0], &it.sub};
      C2D_DrawParams p = {{it.x, it.y, it.w, it.h}, {0, 0}, 0, 0};
      if (!C2D_DrawImage(img, &p, nullptr))
        ok = false;
    }
    C2D_Flush();
    C3D_FrameEnd(0);
    // Outside a frame this waits for rendering and transfer completion.
    // Even on a rejected draw, drain before a caller can free its textures.
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    C3D_SyncDisplayTransfer(
        (u32 *)target->frameBuf.colorBuf, GX_BUFFER_DIM(240, physical_height),
        (u32 *)fb, GX_BUFFER_DIM(w, h),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
            GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    pending = false;
    items.clear();
    last_fb = fb;
    ++generation;
    sync = framebuffer_blit_utils::PhysicalFramebufferSyncState();
    const int slot =
        framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb);
    framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb,
                                                          generation);
    return ok;
  }
  bool Transfer(Text *t) {
    if (!active || text != t)
      return false;
    if (pending)
      return Draw();
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    const int slot =
        framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(&sync, fb);
    if (!framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(sync, slot,
                                                              generation))
      return false;
    C3D_SyncDisplayTransfer((u32 *)target->frameBuf.colorBuf,
                            GX_BUFFER_DIM(240, physical_height), (u32 *)fb,
                            GX_BUFFER_DIM(w, h),
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
    last_fb = fb;
    framebuffer_blit_utils::MarkPhysicalFramebufferCopied(&sync, slot, fb,
                                                          generation);
    return true;
  }
  void ToSoftware(Text *t, bool discard) {
    if (!active || text != t)
      return;
    if (!discard) {
      // Transition-only snapshot: never paid during resident pan/zoom.
      if (pending) {
        Draw();
      }
      if (last_fb) {
        GSPGPU_InvalidateDataCache(last_fb, (u32)(240 * physical_height * 3));
        t->RestoreGpuTop(last_fb, physical_height, orientation);
      }
    }
    active = false;
    pending = false;
    items.clear();
    last_fb = nullptr; // CPU writes can replace this physical buffer now.
    t->screenleft_hw_sync =
        framebuffer_blit_utils::PhysicalFramebufferSyncState();
    t->screenleft_dirty = true;
    t->screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
        0, 0, t->LogicalWidthFor(true), t->LogicalHeightFor(true));
  }
};
static Renderer renderer;

TextureCache::~TextureCache() {
  Reset();
}
TextureCache::TextureCache(TextureCache &&other) : TextureCache() {
  *this = std::move(other);
}
TextureCache &TextureCache::operator=(TextureCache &&other) {
  if (this != &other) {
    Reset();
    for (size_t i = 0; i < renderer.items.size(); ++i)
      if (renderer.items[i].owner == &other)
        renderer.items[i].owner = this;
    impl_ = other.impl_;
    generation_ = other.generation_;
    attempted_ = other.attempted_;
    other.impl_ = nullptr;
    other.attempted_ = false;
  }
  return *this;
}
void TextureCache::Reset() {
  if (impl_)
    renderer.Drop(this, nullptr);
  delete impl_;
  impl_ = nullptr;
  attempted_ = false;
}
bool TextureCache::Upload(Text *t, const uint16_t *pixels, size_t count,
                          int width, int height, int stride,
                          uint64_t generation) {
  return UploadImpl(t, pixels, count, width, height, stride, generation, nullptr);
}
bool TextureCache::UploadRgb(Text *t, const raster_image::Rgb8View &source,
                             uint64_t generation) {
  return UploadImpl(t, nullptr, 0, source.width, source.height, 0, generation, &source);
}
bool TextureCache::UploadImpl(Text *t, const uint16_t *pixels, size_t count,
                              int width, int height, int stride,
                              uint64_t generation,
                              const raster_image::Rgb8View *rgb) {
  if (Matches(generation))
    return true;
  if (attempted_ && generation_ == generation)
    return false;
  Reset();
  attempted_ = true;
  generation_ = generation;
  if ((rgb ? !rgb->Valid() : !pixels) || !renderer.Ensure(t))
    return false;
  Impl *candidate = new (std::nothrow) Impl;
  if (!candidate)
    return false;
  size_t bytes = 0;
  bool ok =
      fixed_layout_tiles::BuildGrid(width, height, &candidate->plan, &bytes);
  candidate->width = width;
  candidate->height = height;
  u16 *staging = nullptr;
  // Group adjacent Morton chunks in a bounded staging allocation. Copies stay
  // synchronous: the same buffer is never overwritten before DMA completes.
  const size_t min_staging = 64u * 1024u;
  const size_t max_staging = 256u * 1024u;
  size_t staging_bytes = std::min(bytes, max_staging);
  const uint64_t started = fixed_perf::Now();
  for (size_t i = 0; ok && i < candidate->plan.size(); ++i) {
    const fixed_layout_tiles::Tile &tile = candidate->plan[i];
    C3D_Tex tex = {};
    const size_t size = (size_t)tile.tex_width * tile.tex_height * 2;
    const bool vram =
        C3D_TexInitVRAM(&tex, tile.tex_width, tile.tex_height, GPU_RGB565);
    if (!vram &&
        (linearSpaceFree() < size + 1024u * 1024u ||
         !C3D_TexInit(&tex, tile.tex_width, tile.tex_height, GPU_RGB565))) {
      ok = false;
      break;
    }
    candidate->textures.push_back(tex);
    if (vram && !staging) {
      if (linearSpaceFree() < staging_bytes + 1024u * 1024u)
        staging_bytes = std::min(bytes, min_staging);
      if (linearSpaceFree() < staging_bytes + 1024u * 1024u) {
        ok = false;
        break;
      }
      staging = (u16 *)linearMemAlign(staging_bytes, 128);
      if (!staging && staging_bytes > min_staging) {
        staging_bytes = min_staging;
        staging = (u16 *)linearMemAlign(staging_bytes, 128);
      }
      if (!staging) {
        ok = false;
        break;
      }
    }
    const size_t chunk_pixels = vram ? staging_bytes / 2 : size / 2;
    for (size_t first = 0; ok && first < size / 2; first += chunk_pixels) {
      const size_t n = std::min(chunk_pixels, size / 2 - first);
      u16 *dst = vram ? staging : (u16 *)tex.data + first;
      ok = rgb ? fixed_layout_tiles::SwizzleRgb8Chunk(tile, *rgb, first, n, dst)
               : fixed_layout_tiles::SwizzleChunk(tile, pixels, count, stride,
                                                   width, height, first, n, dst);
      if (ok && vram) {
        GSPGPU_FlushDataCache(staging, n * 2);
        C3D_SyncTextureCopy((u32 *)staging, 0, (u32 *)((u16 *)tex.data + first),
                            0, n * 2, 8);
      }
    }
    C3D_TexSetFilter(&candidate->textures.back(), GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&candidate->textures.back(), GPU_CLAMP_TO_EDGE,
                   GPU_CLAMP_TO_EDGE);
    if (!vram)
      C3D_TexFlush(&candidate->textures.back());
  }
  if (staging)
    linearFree(staging);
  fixed_perf::Record(this, -1, -1, "gpu.upload", fixed_perf::Now() - started,
                     ok ? 1 : 0, bytes, width, height);
  if (!ok) {
    delete candidate;
    return false;
  }
  impl_ = candidate;
  return true;
}
TileTexture::~TileTexture() {
  Reset();
}
TileTexture::TileTexture(TileTexture &&other) : TileTexture() {
  *this = std::move(other);
}
TileTexture &TileTexture::operator=(TileTexture &&other) {
  if (this != &other) {
    Reset();
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}
void TileTexture::Reset() {
  if (!impl_)
    return;
  renderer.Drop(nullptr, &impl_->tex);
  C3D_TexDelete(&impl_->tex);
  delete impl_;
  impl_ = nullptr;
}
bool TileTexture::Create(Text *t, int width, int height) {
  Reset();
  if (width < 8 || height < 8 || (width & (width - 1)) ||
      (height & (height - 1)) || !renderer.Ensure(t))
    return false;
  Impl *i = new (std::nothrow) Impl;
  if (!i)
    return false;
  i->width = width;
  i->height = height;
  i->tex = C3D_Tex();
  const size_t size = (size_t)width * height * 2;
  i->vram = C3D_TexInitVRAM(&i->tex, width, height, GPU_RGB565);
  if (!i->vram && (linearSpaceFree() < size + 1024u * 1024u ||
                   !C3D_TexInit(&i->tex, width, height, GPU_RGB565))) {
    delete i;
    return false;
  }
  C3D_TexSetFilter(&i->tex, GPU_LINEAR, GPU_LINEAR);
  C3D_TexSetWrap(&i->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
  impl_ = i;
  return true;
}
bool TileTexture::UploadHardware(const void *linear, int width, int height) {
  if (!impl_ || !linear || width != impl_->width || height != impl_->height ||
      ((uintptr_t)linear & 15))
    return false;
  const uint64_t started = fixed_perf::Now();
  GSPGPU_FlushDataCache(const_cast<void *>(linear), (size_t)width * height * 3);
  // Linear RGB8 rows in, 8x8-tiled (Morton) RGB565 out: the layout the PICA
  // texture unit samples. No vertical flip: row 0 stays texture row 0, exactly
  // as the CPU swizzle writes it.
  C3D_SyncDisplayTransfer(
      (u32 *)linear, GX_BUFFER_DIM(width, height), (u32 *)impl_->tex.data,
      GX_BUFFER_DIM(width, height),
      GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
          GX_TRANSFER_RAW_COPY(0) |
          GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) |
          GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
          GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  fixed_perf::Record(this, -1, -1, "gpu.upload_hw", fixed_perf::Now() - started,
                     1, (size_t)width * height * 2, width, height);
  return true;
}
bool TileTexture::UploadRgb8(const raster_image::Rgb8View &source) {
  if (!impl_ || !source.Valid() || source.width != impl_->width ||
      source.height != impl_->height)
    return false;
  const uint64_t started = fixed_perf::Now();
  // The source already carries the halo: texture (x,y) = source (x,y).
  const fixed_layout_tiles::Tile tile = {1, 1, impl_->width - 2,
                                         impl_->height - 2, impl_->width,
                                         impl_->height};
  const size_t total = (size_t)impl_->width * impl_->height;
  bool ok = true;
  if (!impl_->vram) {
    ok = fixed_layout_tiles::SwizzleRgb8Chunk(tile, source, 0, total,
                                              (u16 *)impl_->tex.data);
    if (ok)
      C3D_TexFlush(&impl_->tex);
  } else {
    const size_t chunk = std::min<size_t>(total, 16u * 1024u);
    u16 *staging = linearSpaceFree() > chunk * 2 + 1024u * 1024u
                       ? (u16 *)linearMemAlign(chunk * 2, 128)
                       : nullptr;
    ok = staging != nullptr;
    for (size_t first = 0; ok && first < total; first += chunk) {
      const size_t n = std::min(chunk, total - first);
      ok = fixed_layout_tiles::SwizzleRgb8Chunk(tile, source, first, n, staging);
      if (ok) {
        GSPGPU_FlushDataCache(staging, n * 2);
        C3D_SyncTextureCopy((u32 *)staging, 0,
                            (u32 *)((u16 *)impl_->tex.data + first), 0, n * 2, 8);
      }
    }
    if (staging)
      linearFree(staging);
  }
  fixed_perf::Record(this, -1, -1, "gpu.upload_cpu", fixed_perf::Now() - started,
                     ok ? 1 : 0, total * 2, impl_->width, impl_->height);
  return ok;
}
void *AllocLinearScratch(size_t bytes) {
  if (linearSpaceFree() < bytes + 1024u * 1024u)
    return nullptr;
  return linearMemAlign(bytes, 128);
}
void FreeLinearScratch(void *p) {
  if (p)
    linearFree(p);
}
bool QueueTiles(Text *t, const void *owner, const TileDraw *draws, size_t count,
                bool append) {
  return renderer.EnqueueTiles(t, owner, draws, count, append);
}
bool Queue(Text *t, TextureCache &c, const fixed_layout_tiles::Rect &s,
           const fixed_layout_tiles::Rect &v,
           const fixed_layout_tiles::Rect &d, int quarter_turns) {
  return renderer.Enqueue(t, c, s, v, d, quarter_turns);
}
bool Active(const Text *t) {
  return renderer.text == t && renderer.active;
}
bool WillDraw(const Text *t) {
  return Active(t) && renderer.pending;
}
bool NeedsPresent(Text *t) {
  if (!Active(t))
    return false;
  if (renderer.pending)
    return true;
  u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
  const int slot = framebuffer_blit_utils::ResolvePhysicalFramebufferSlot(
      &renderer.sync, fb);
  return framebuffer_blit_utils::NeedsPhysicalFramebufferCopy(
      renderer.sync, slot, renderer.generation);
}
bool Present(Text *t) {
  return renderer.Transfer(t);
}
void Software(Text *t, bool discard) {
  renderer.ToSoftware(t, discard);
}
void Shutdown(Text *t) {
  if (renderer.text != t)
    return;
  renderer.ToSoftware(t, true);
  renderer.background.Reset();
  if (renderer.target)
    C3D_RenderTargetDelete(renderer.target);
  renderer.target = nullptr;
  if (renderer.initialized) {
    C2D_Fini();
    C3D_Fini();
  }
  renderer.initialized = false;
  renderer.text = nullptr;
}
} // namespace fixed_layout_gpu
#endif
