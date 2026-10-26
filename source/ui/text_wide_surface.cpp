#include "ui/text.h"

#include <new>

#include "shared/debug_log.h"

bool Text::IsWideTopScreenCapable() {
  if (wide_top_capable_ < 0) {
    u8 model = 0xFF;
    wide_top_capable_ = 0;
    if (!R_FAILED(cfguInit())) {
      const Result result = CFGU_GetSystemModel(&model);
      cfguExit();
      if (!R_FAILED(result) && model <= 5 && model != CFG_MODEL_2DS)
        wide_top_capable_ = 1;
    }
  }
  return wide_top_capable_ != 0;
}

bool Text::SetTopScreenWideMode(bool wide) {
  if (wide == IsWideTopScreenActive())
    return true;
  if (wide) {
    if (!IsWideTopScreenCapable())
      return false;
    u16 *pixels = new (std::nothrow) u16[240 * 800];
    if (!pixels) {
      DBG_LOGF(reporter_, "WIDE: enable failed bytes=%u", 240u * 800u * 2u);
      return false;
    }
    u8 *conversion = new (std::nothrow) u8[240 * 800 * 3];
    if (!conversion) {
      delete[] pixels;
      DBG_LOGF(reporter_, "WIDE: enable failed conversion_bytes=%u", 240u * 800u * 3u);
      return false;
    }
    wide_top_pixels_ = pixels;
    wide_top_fb_cache_ = conversion;
  } else {
    delete[] wide_top_pixels_;
    wide_top_pixels_ = nullptr;
    delete[] wide_top_fb_cache_;
    wide_top_fb_cache_ = nullptr;
  }
  // libctru applies this flag on the next framebuffer presentation. Its top
  // hardware buffers already have room for 800 columns. Main thread only.
  gfxSetWide(wide);
  wide_top_valid_ = false;
  wide_top_ui_dirty_.valid = false;
  screenleft_hw_sync = framebuffer_blit_utils::PhysicalFramebufferSyncState();
  MarkScreenDirty(screenleft);
  DBG_LOGF(reporter_, "WIDE: active=%d buffer_bytes=%u logical=%dx%d",
           wide ? 1 : 0, wide ? 240u * 800u * 5u : 0u,
           LogicalWidthFor(true), LogicalHeightFor(true));
  return true;
}

u16 *Text::BeginWideTopBlit(int x, int y, int width, int height) {
  if (!wide_top_pixels_)
    return nullptr;
  // Apply pending UI pixels before replacing this region with native detail.
  SyncWideTopUiRect();
  const bool was_valid = wide_top_valid_;
  MarkScreenDirtyRect(screenleft, x, y, x + width, y + height);
  if (!was_valid) {
    const bool landscape = orientation_utils::IsLandscape(GetOrientation());
    const int dx = landscape ? 2 : 1;
    const int dy = landscape ? 1 : 2;
    const int logical_width = LogicalWidthFor(true);
    const int logical_height = LogicalHeightFor(true);
    const int stride = logical_width * dx;
    for (int sy = 0; sy < logical_height; ++sy) {
      for (int sx = 0; sx < logical_width; ++sx) {
        const u16 pixel = screenleft[sy * BufferStride() + sx];
        for (int yy = 0; yy < dy; ++yy)
          for (int xx = 0; xx < dx; ++xx)
            wide_top_pixels_[(sy * dy + yy) * stride + sx * dx + xx] = pixel;
      }
    }
  }
  wide_top_valid_ = true;
  wide_top_ui_dirty_.valid = false;
  return wide_top_pixels_;
}

void Text::SyncWideTopUiRect() {
  if (!wide_top_pixels_ || !wide_top_valid_ || !wide_top_ui_dirty_.valid)
    return;
  const bool landscape = orientation_utils::IsLandscape(GetOrientation());
  const int dx = landscape ? 2 : 1, dy = landscape ? 1 : 2;
  const int stride = LogicalWidthFor(true) * dx;
  for (int y = wide_top_ui_dirty_.y0; y < wide_top_ui_dirty_.y1; ++y) {
    for (int x = wide_top_ui_dirty_.x0; x < wide_top_ui_dirty_.x1; ++x) {
      const u16 pixel = screenleft[y * BufferStride() + x];
      for (int yy = 0; yy < dy; ++yy)
        for (int xx = 0; xx < dx; ++xx)
          wide_top_pixels_[(y * dy + yy) * stride + x * dx + xx] = pixel;
    }
  }
  wide_top_ui_dirty_.valid = false;
}
