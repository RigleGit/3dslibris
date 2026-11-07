#pragma once
#include <citro2d.h>
#include "shared/orientation_utils.h"
#include "ui/framebuffer_blit_utils.h"
class Text {
public:
  int orientation=0;
  u16 left[400 * 400]{}, right[400 * 400]{};
  u16 *screenleft = left, *screenright = right, *screen = left;
  bool screenleft_dirty = false, dirty_bottom = false;
  uint64_t screenleft_cache_generation = 0;
  framebuffer_blit_utils::DirtyRect screenleft_dirty_rect{};
  framebuffer_blit_utils::PhysicalFramebufferSyncState screenleft_hw_sync;
  int BufferStride() const {
    return 400;
  }
  int LogicalWidthFor(bool) const {
    return orientation==2 ? 400 : 240;
  }
  int LogicalHeightFor(bool) const {
    return orientation==2 ? 240 : 400;
  }
  int GetColorMode() const {
    return 0;
  }
  int GetOrientation() const {
    return orientation;
  }
  bool IsWideTopScreenActive() const {
    return gpu.wide;
  }
  u16 *GetScreen() const {
    return screen;
  }
  void SetScreen(u16 *p) {
    screen = p;
  }
  void ClearScreen() {
    std::memset(screen, 255, sizeof(left));
  }
  void RestoreGpuTop(const u8 *, int, int) {
    ++gpu.restores;
  }
  bool HasDirtyScreens() const {
    return dirty_bottom || screenleft_dirty;
  }
  bool BlitToFramebuffer() {
    bool dirty = dirty_bottom;
    dirty_bottom = false;
    return dirty;
  }
};
