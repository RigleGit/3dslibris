#pragma once

namespace heading_layout {

struct KeepWithNextRequest {
  int pen_y;
  int screen_height;
  int bottom_margin;
  int line_height;
  int linespacing;
  int heading_level;
};

bool ShouldAdvanceHeadingForKeepWithNext(const KeepWithNextRequest &req);

} // namespace heading_layout
