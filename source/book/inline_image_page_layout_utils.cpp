#include "book/inline_image_page_layout_utils.h"

#include "shared/aspect_fit_utils.h"

#include <algorithm>

namespace {

static int ClampPositive(int value, int fallback) {
  return (value > 0) ? value : fallback;
}

} // namespace

InlineImagePagePlacement ResolveInlineImagePagePlacement(
    int screen_width, int screen_height, int margin_left, int margin_right,
    int margin_top, int margin_bottom, int src_width, int src_height,
    int horizontal_padding, int top_padding, int bottom_padding) {
  InlineImagePagePlacement out{};

  screen_width = ClampPositive(screen_width, 240);
  screen_height = ClampPositive(screen_height, 320);
  src_width = ClampPositive(src_width, 1);
  src_height = ClampPositive(src_height, 1);
  horizontal_padding = std::max(0, horizontal_padding);
  top_padding = std::max(0, top_padding);
  bottom_padding = std::max(0, bottom_padding);

  const int left = std::max(0, margin_left) + horizontal_padding;
  const int top = std::max(0, margin_top) + top_padding;
  const int right = std::max(
      left + 1, screen_width - std::max(0, margin_right) - horizontal_padding);
  const int bottom = std::max(
      top + 1, screen_height - std::max(0, margin_bottom) - bottom_padding);

  out.avail_width = std::max(1, right - left);
  out.avail_height = std::max(1, bottom - top);

  // PAGE images/covers should fill the available page box.
  // Unlike inline/band images, page placement is allowed to upscale.
  const aspect_fit_utils::Placement placement =
      aspect_fit_utils::FitInsideBox(left, top, out.avail_width,
                                     out.avail_height, src_width, src_height,
                                     true);
  out.draw_width = placement.width;
  out.draw_height = placement.height;
  out.start_x = placement.x;
  out.start_y = placement.y;
  return out;
}
