#include "settings/go_to_page_slider_utils.h"

namespace settings {

int ClampGoToPageTarget(int target_page, int page_count) {
  if (page_count <= 0)
    return 0;
  if (target_page < 0)
    return 0;
  if (target_page >= page_count)
    return page_count - 1;
  return target_page;
}

int SliderTouchXToPageIndex(int touch_x, int slider_x, int slider_w,
                            int page_count) {
  if (page_count <= 1 || slider_w <= 0)
    return 0;

  const int clamped_x = touch_x < slider_x
                            ? slider_x
                            : (touch_x > slider_x + slider_w ? slider_x + slider_w
                                                             : touch_x);
  const int relative_x = clamped_x - slider_x;
  const int max_page = page_count - 1;
  return (relative_x * max_page) / slider_w;
}

int SliderPageIndexToFillWidth(int current_page, int page_count, int slider_w) {
  if (page_count <= 1 || slider_w <= 0)
    return 0;

  const int clamped_page = ClampGoToPageTarget(current_page, page_count);
  const int max_page = page_count - 1;
  return (clamped_page * slider_w) / max_page;
}

} // namespace settings
