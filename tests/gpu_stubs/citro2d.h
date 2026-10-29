#pragma once
// Minimal SDK boundary. The production cache/presenter run unchanged; these
// operations expose allocation lifetimes, transfer bytes and synchronization.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct GpuBoundary {
  int full_flushes = 0, framebuffer_flushes = 0, copies = 0, frames = 0,
      restores = 0, swaps = 0;
  int live_textures = 0, allocation_attempts = 0, fail_after = -1;
  bool fail_frame = false;
  int target_max_textures = 1000;
  bool wide = false;
  size_t linear_free = 16u * 1024u * 1024u;
  size_t max_linear_block = 16u * 1024u * 1024u;
  std::map<void *, size_t> linear_blocks;
  std::map<void *, size_t> textures;
  std::vector<size_t> transfers;
  u8 top[2][240 * 800 * 3]{};
  int slot = 0;
};
extern GpuBoundary gpu;
enum {
  GFX_TOP,
  GFX_LEFT,
  GPU_RB_RGB8,
  GPU_RGB565,
  GPU_LINEAR,
  GPU_CLAMP_TO_EDGE,
  GPU_ALWAYS,
  GPU_WRITE_COLOR
};
const size_t C3D_DEFAULT_CMDBUF_SIZE = 256 * 1024;
#define GX_BUFFER_DIM(w, h) ((u32)(w) | ((u32)(h) << 16))
#define GX_TRANSFER_FLIP_VERT(x) 0
#define GX_TRANSFER_OUT_TILED(x) 0
#define GX_TRANSFER_RAW_COPY(x) 0
#define GX_TRANSFER_IN_FORMAT(x) 0
#define GX_TRANSFER_OUT_FORMAT(x) 0
#define GX_TRANSFER_SCALING(x) 0
#define GX_TRANSFER_FMT_RGB8 0
#define GX_TRANSFER_SCALE_NO 0
struct C3D_DEPTHTYPE {
  explicit C3D_DEPTHTYPE(int) {}
};
struct C3D_Tex {
  void *data;
  int width, height;
  bool vram;
};
struct C3D_RenderTarget {
  struct {
    void *colorBuf;
  } frameBuf;
};
struct Tex3DS_SubTexture {
  u16 width, height;
  float left, top, right, bottom;
};
struct C2D_Image {
  C3D_Tex *tex;
  const Tex3DS_SubTexture *subtex;
};
struct C2D_DrawParams {
  struct {
    float x, y, w, h;
  } pos;
  struct {
    float x, y;
  } center;
  float depth, angle;
};
inline bool C3D_Init(size_t) {
  return true;
}
inline bool C2D_Init(size_t) {
  return true;
}
inline void C2D_Prepare() {}
inline void C2D_Fini() {}
inline void C3D_Fini() {}
inline C3D_RenderTarget *C3D_RenderTargetCreate(int w, int h, int,
                                                C3D_DEPTHTYPE) {
  if (gpu.live_textures > gpu.target_max_textures)
    return nullptr;
  auto *t = new C3D_RenderTarget;
  t->frameBuf.colorBuf = std::calloc((size_t)w * h, 3);
  return t;
}
inline void C3D_RenderTargetDelete(C3D_RenderTarget *t) {
  std::free(t->frameBuf.colorBuf);
  delete t;
}
inline bool C3D_FrameBegin(int flags) {
  assert(flags == 0);
  ++gpu.frames;
  return !gpu.fail_frame;
}
inline void C3D_FrameEnd(int flags) {
  assert(flags == 0);
  ++gpu.full_flushes;
}
inline void C3D_FrameDrawOn(C3D_RenderTarget *) {}
inline void C3D_DepthTest(bool, int, int) {}
inline void C2D_TargetClear(C3D_RenderTarget *, u32) {}
inline u32 C2D_Color32(int, int, int, int) {
  return 0;
}
inline void C2D_SceneSize(int, int, bool) {}
inline void C2D_ViewReset() {}
inline void C2D_ViewTranslate(int, int) {}
inline void C2D_ViewRotateDegrees(int) {}
inline bool C2D_DrawImage(C2D_Image img, const C2D_DrawParams *, const void *) {
  assert(gpu.textures.count(img.tex->data));
  return true;
}
inline bool C2D_DrawImageAt(C2D_Image img, int, int, int) {
  return C2D_DrawImage(img, nullptr, nullptr);
}
inline void C2D_Flush() {}
inline void gfxFlushBuffers() {
  ++gpu.framebuffer_flushes;
}
inline void gfxSwapBuffers() {
  ++gpu.swaps;
  gpu.slot ^= 1;
}
inline u8 *gfxGetFramebuffer(int, int, u16 *w, u16 *h) {
  if (w)
    *w = 240;
  if (h)
    *h = gpu.wide ? 800 : 400;
  return gpu.top[gpu.slot];
}
inline void GSPGPU_FlushDataCache(void *, size_t) {}
inline void GSPGPU_InvalidateDataCache(void *, size_t) {}
inline void C3D_SyncDisplayTransfer(u32 *, u32, u32 *out, u32 dim, int) {
  ++gpu.copies;
  std::memset(out, 0x5a, (dim & 65535) * (dim >> 16) * 3);
}
inline void C3D_SyncTextureCopy(u32 *src, int, u32 *out, int, size_t size,
                                int) {
  gpu.transfers.push_back(size);
  std::memcpy(out, src, size);
}
inline size_t linearSpaceFree() {
  return gpu.linear_free;
}
inline void *linearMemAlign(size_t bytes, size_t alignment) {
  if (bytes > gpu.linear_free || bytes > gpu.max_linear_block)
    return nullptr;
  void *p = nullptr;
  if (posix_memalign(&p, alignment, bytes))
    return nullptr;
  gpu.linear_blocks[p] = bytes;
  gpu.linear_free -= bytes;
  return p;
}
inline void linearFree(void *p) {
  gpu.linear_free += gpu.linear_blocks.at(p);
  gpu.linear_blocks.erase(p);
  std::free(p);
}
inline bool InitTex(C3D_Tex *t, int w, int h, bool vram) {
  if (gpu.fail_after >= 0 && gpu.allocation_attempts++ >= gpu.fail_after)
    return false;
  const size_t size = (size_t)w * h * 2;
  void *p = vram ? std::malloc(size) : linearMemAlign(size, 128);
  if (!p)
    return false;
  *t = {p, w, h, vram};
  gpu.textures[p] = size;
  ++gpu.live_textures;
  return true;
}
inline bool C3D_TexInitVRAM(C3D_Tex *t, int w, int h, int) {
  return InitTex(t, w, h, true);
}
inline bool C3D_TexInit(C3D_Tex *t, int w, int h, int) {
  return InitTex(t, w, h, false);
}
inline void C3D_TexSetFilter(C3D_Tex *, int, int) {}
inline void C3D_TexSetWrap(C3D_Tex *, int, int) {}
inline void C3D_TexFlush(C3D_Tex *) {}
inline void C3D_TexDelete(C3D_Tex *t) {
  assert(gpu.textures.erase(t->data) == 1);
  --gpu.live_textures;
  if (t->vram)
    std::free(t->data);
  else
    linearFree(t->data);
}
