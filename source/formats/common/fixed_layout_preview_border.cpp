#include "formats/common/fixed_layout_preview_panel.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "ui/text.h"
#include <algorithm>

namespace fixed_layout_preview_panel {
void RestoreRect(Text *text, const ContentKey &content, int x0, int y0, int x1,
                 int y1) {
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(content.screen_width, x1);
  y1 = std::min(content.screen_height, y1);
  if (x0 >= x1 || y0 >= y1)
    return;
  text->MarkScreenDirtyRect(content.screen, x0, y0, x1, y1);
  if (content.pixels) {
    fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(
        content.screen, content.stride, content.screen_width,
        content.screen_height, content.layout.x, content.layout.y,
        content.layout.width, content.layout.height, content.pixels,
        content.pixel_count, content.bitmap_width, content.bitmap_height, 0, 0,
        content.bitmap_width, content.bitmap_height, true, x0, y0, x1, y1,
        content.quarter_turns);
  } else {
    text->FillRect((u16)x0, (u16)y0, (u16)x1, (u16)y1,
                   fixed_layout_preview::kPaper);
  }
}
void RestoreBorder(Text *text, const ContentKey &content, const Rect &old) {
  RestoreRect(text, content, old.x, old.y, old.x + old.width, old.y + 1);
  if (old.height > 1)
    RestoreRect(text, content, old.x, old.y + old.height - 1, old.x + old.width,
                old.y + old.height);
  if (old.height > 2) {
    RestoreRect(text, content, old.x, old.y + 1, old.x + 1,
                old.y + old.height - 1);
    if (old.width > 1)
      RestoreRect(text, content, old.x + old.width - 1, old.y + 1,
                  old.x + old.width, old.y + old.height - 1);
  }
}
} // namespace fixed_layout_preview_panel
