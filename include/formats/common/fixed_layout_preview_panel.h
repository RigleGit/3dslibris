#pragma once

#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/pdf_view_utils.h"

#include <stdint.h>

class Book;
class Text;

namespace fixed_layout_preview_panel {

struct Rect {
  int x, y, width, height;
  bool operator==(const Rect &other) const {
    return x == other.x && y == other.y && width == other.width &&
           height == other.height;
  }
};

struct ContentKey {
  const u16 *pixels;
  u16 *screen;
  size_t pixel_count;
  int bitmap_width, bitmap_height;
  int page, zoom, color_mode, orientation;
  int screen_width, screen_height, stride;
  bool wide_top;
  Rect layout;
  int quarter_turns; // Zero when omitted from aggregate initialization.

  bool operator==(const ContentKey &other) const;
};

// Only geometry and identity are retained; the preview remains the pixel owner.
struct State {
  bool valid;
  ContentKey content;
  Rect viewport;
  uint64_t expected_generation;

  State() : valid(false), content(), viewport(), expected_generation(0) {}
};

// Shared pixel restoration; callers keep ownership of backgrounds and controls.
void RestoreRect(Text *text, const ContentKey &content,
                 int x0, int y0, int x1, int y1);
void RestoreBorder(Text *text, const ContentKey &content, const Rect &old);

void Draw(Book *book, Text *text, State *state,
          const u16 *pixels, size_t pixel_count, int bitmap_width,
          int bitmap_height, int page, int zoom, bool wide_top,
          const pdf_view_utils::PreviewLayout &layout, const Rect &viewport);

} // namespace fixed_layout_preview_panel
