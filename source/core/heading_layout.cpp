#include "heading_layout.h"

#include <algorithm>

namespace heading_layout {

bool ShouldAdvanceHeadingForKeepWithNext(const KeepWithNextRequest &req) {
  if (req.pen_y <= 0 || req.screen_height <= 0 || req.line_height <= 0)
    return false;

  int total_lines = 2;
  if (req.heading_level <= 1)
    total_lines = 3;

  const int line_step = req.line_height + std::max(0, req.linespacing);
  const int max_y = req.screen_height - std::max(0, req.bottom_margin);
  const int needed_bottom =
      req.pen_y + ((total_lines - 1) * line_step) + req.line_height;
  return needed_bottom > max_y;
}

} // namespace heading_layout
