#include "book/inline_image_screen_layout.h"

#include "shared/text_render_layout_utils.h"

InlineImageScreenLayout ResolveInlineImageScreenLayout(
    bool current_screen_is_left, int full_bottom_margin) {
  const int compact_bottom_margin =
      text_render_layout_utils::ResolveCompactReadingBottomMargin(
          full_bottom_margin);
  const int guarded_full_bottom_margin =
      full_bottom_margin +
      text_render_layout_utils::kFullReadingScreenFooterGuardPx;

  InlineImageScreenLayout layout{};
  if (current_screen_is_left) {
    layout.current_screen_height = 400;
    layout.current_margin_bottom = guarded_full_bottom_margin;
    layout.next_screen_height = 320;
    layout.next_margin_bottom = compact_bottom_margin;
  } else {
    layout.current_screen_height = 320;
    layout.current_margin_bottom = compact_bottom_margin;
    layout.next_screen_height = 400;
    layout.next_margin_bottom = guarded_full_bottom_margin;
  }
  return layout;
}

InlineImageScreenLayout ResolveInlineImageScreenLayoutForReadingScreen(
    bool turned_right, int current_screen, int full_bottom_margin) {
  const bool current_screen_is_left =
      turned_right ? (current_screen != 0) : (current_screen == 0);
  return ResolveInlineImageScreenLayout(current_screen_is_left,
                                        full_bottom_margin);
}
