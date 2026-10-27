#include "formats/common/fixed_layout_preview_panel.h"

#include "book/book.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "ui/text.h"

#include <algorithm>

namespace fixed_layout_preview_panel {

bool ContentKey::operator==(const ContentKey &other) const {
  return pixels == other.pixels && screen == other.screen &&
         pixel_count == other.pixel_count && bitmap_width == other.bitmap_width &&
         bitmap_height == other.bitmap_height && page == other.page &&
         zoom == other.zoom && color_mode == other.color_mode &&
         orientation == other.orientation && screen_width == other.screen_width &&
         screen_height == other.screen_height && stride == other.stride &&
         wide_top == other.wide_top && layout == other.layout;
}

static bool Contains(const Rect &outer, const Rect &inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.width <= outer.x + outer.width &&
         inner.y + inner.height <= outer.y + outer.height;
}

static void RestoreRect(Text *text, const ContentKey &content,
                        int x0, int y0, int x1, int y1) {
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
        content.layout.width, content.layout.height,
        content.pixels, content.pixel_count, content.bitmap_width,
        content.bitmap_height, 0, 0, content.bitmap_width, content.bitmap_height,
        true, x0, y0, x1, y1);
  } else {
    text->FillRect((u16)x0, (u16)y0, (u16)x1, (u16)y1,
                   fixed_layout_preview::kPaper);
  }
}

static void DrawOutline(Text *text, const Rect &rect, u16 color) {
  text->DrawRect((u16)rect.x, (u16)rect.y,
                 (u16)(rect.x + rect.width),
                 (u16)(rect.y + rect.height), color);
}

void Draw(Book *book, Text *text, State *state,
          const u16 *pixels, size_t pixel_count, int bitmap_width,
          int bitmap_height, int page, int zoom, bool wide_top,
          const pdf_view_utils::PreviewLayout &layout, const Rect &viewport) {
  if (!book || !text || !state)
    return;
  const bool valid_pixels = pixels && bitmap_width > 0 && bitmap_height > 0 &&
      pixel_count >= (size_t)bitmap_width * (size_t)bitmap_height;
  const ContentKey content = {
      valid_pixels ? pixels : NULL, text->screenright, pixel_count,
      bitmap_width, bitmap_height, page, zoom, text->GetColorMode(),
      text->GetOrientation(), text->LogicalWidthFor(false),
      text->LogicalHeightFor(false), text->BufferStride(), wide_top,
      {layout.x, layout.y, layout.width, layout.height}};

  // Dirty is pending conversion, not a record of who last changed the buffer.
  // The generation also detects a menu that was already drawn and presented.
  // A normal presentation consumes exactly the one generation we expect here;
  // changes on the top screen alone do not invalidate the bottom preview.
  const bool content_changed = !state->valid || !(state->content == content) ||
      text->screenright_dirty ||
      state->expected_generation != text->screenright_cache_generation;
  if (!content_changed && state->viewport == viewport)
    return;
  // Independent rounding of origin and size can put an accent one pixel
  // outside the preview. Its old pixels also need the gradient on a move.
  const bool full_redraw = content_changed ||
      !Contains(content.layout, state->viewport);

  text->SetScreen(text->screenright);
  if (full_redraw) {
    if (!book->DrawBottomGradientBackground())
      text->ClearScreen();
    else
      text->MarkScreenDirty(text->screenright);
    RestoreRect(text, content, layout.x, layout.y,
                layout.x + layout.width, layout.y + layout.height);
  } else {
    // Restore only the four old border strips, preserving the full preview's
    // sampling origin and scale. Corners are included in the horizontal strips.
    const Rect &old = state->viewport;
    RestoreRect(text, content, old.x, old.y,
                old.x + old.width, old.y + 1);
    if (old.height > 1)
      RestoreRect(text, content, old.x, old.y + old.height - 1,
                  old.x + old.width, old.y + old.height);
    if (old.height > 2) {
      RestoreRect(text, content, old.x, old.y + 1,
                  old.x + 1, old.y + old.height - 1);
      if (old.width > 1)
        RestoreRect(text, content, old.x + old.width - 1, old.y + 1,
                    old.x + old.width, old.y + old.height - 1);
    }
  }
  // An old accent may overlap the preview frame, so restore the frame before
  // drawing the new accent, in the same order as the full redraw.
  DrawOutline(text, content.layout, fixed_layout_preview::kFrame);
  DrawOutline(text, viewport, fixed_layout_preview::kViewportAccent);
  state->content = content;
  state->viewport = viewport;
  state->valid = true;
  state->expected_generation = text->screenright_cache_generation + 1;
}

} // namespace fixed_layout_preview_panel
