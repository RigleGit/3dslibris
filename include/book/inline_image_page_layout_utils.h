#pragma once

struct InlineImagePagePlacement {
  int draw_width;
  int draw_height;
  int start_x;
  int start_y;
  int avail_width;
  int avail_height;
};

InlineImagePagePlacement ResolveInlineImagePagePlacement(
    int screen_width, int screen_height, int margin_left, int margin_right,
    int margin_top, int margin_bottom, int src_width, int src_height,
    int padding = 2);
