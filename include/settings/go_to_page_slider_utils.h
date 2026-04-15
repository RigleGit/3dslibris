#pragma once

namespace settings {

int ClampGoToPageTarget(int target_page, int page_count);
int SliderTouchXToPageIndex(int touch_x, int slider_x, int slider_w,
                            int page_count);
int SliderPageIndexToFillWidth(int current_page, int page_count, int slider_w);

} // namespace settings
