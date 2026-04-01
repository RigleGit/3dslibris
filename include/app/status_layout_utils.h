#pragma once

namespace status_layout_utils {

struct BookStatusHudLayout {
  int text_y;
  int clear_top;
  int clear_bottom;
  int progress_bar_y;
  int progress_bar_height;
};

struct FixedLayoutBottomHudLayout {
  int time_y;
  int time_clear_top;
  int time_clear_bottom;
  int page_y;
  int page_clear_top;
  int page_clear_bottom;
  int right_margin;
};

inline BookStatusHudLayout ComputeBookStatusHudLayout(int screen_height,
                                                      int font_height,
                                                      int footer_reserved_height) {
  BookStatusHudLayout layout = {};
  if (screen_height <= 0)
    return layout;

  const int bottom_padding = 13;
  layout.progress_bar_height = 8;
  layout.text_y = screen_height - 25;
  layout.progress_bar_y = layout.text_y + 4;

  const int progress_bottom = layout.progress_bar_y + layout.progress_bar_height;
  const int max_progress_bottom = screen_height - bottom_padding;
  if (progress_bottom > max_progress_bottom) {
    const int shift = progress_bottom - max_progress_bottom;
    layout.text_y -= shift;
    layout.progress_bar_y -= shift;
  }

  const int reserved_top = screen_height - footer_reserved_height;
  layout.clear_top = layout.text_y - font_height - 8;
  if (layout.clear_top < reserved_top)
    layout.clear_top = reserved_top;
  if (layout.clear_top < 0)
    layout.clear_top = 0;
  layout.clear_bottom = screen_height;
  return layout;
}

inline FixedLayoutBottomHudLayout
ComputeFixedLayoutBottomHudLayout(int screen_height, int font_height) {
  FixedLayoutBottomHudLayout layout = {};
  if (screen_height <= 0)
    return layout;

  layout.right_margin = 8;
  layout.time_y = 10;
  layout.time_clear_top = 0;
  layout.time_clear_bottom = layout.time_y + 8;

  layout.page_y = screen_height - font_height - 10;
  if (layout.page_y < 0)
    layout.page_y = 0;
  layout.page_clear_top = layout.page_y - 9;
  if (layout.page_clear_top < 0)
    layout.page_clear_top = 0;
  layout.page_clear_bottom = layout.page_y + 8;
  if (layout.page_clear_bottom > screen_height)
    layout.page_clear_bottom = screen_height;
  return layout;
}

} // namespace status_layout_utils
