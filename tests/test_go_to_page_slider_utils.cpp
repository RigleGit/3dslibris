#include "settings/go_to_page_slider_utils.h"

#include "test_assert.h"

int main() {
  test::ExpectEq("clamp negative target", settings::ClampGoToPageTarget(-5, 120),
                 0);
  test::ExpectEq("clamp target above range",
                 settings::ClampGoToPageTarget(999, 120), 119);
  test::ExpectEq("clamp target inside range",
                 settings::ClampGoToPageTarget(45, 120), 45);
  test::ExpectEq("empty page count clamps to zero",
                 settings::ClampGoToPageTarget(10, 0), 0);

  test::ExpectEq("touch at left edge maps to first page",
                 settings::SliderTouchXToPageIndex(20, 20, 180, 200), 0);
  test::ExpectEq("touch before slider clamps to first page",
                 settings::SliderTouchXToPageIndex(1, 20, 180, 200), 0);
  test::ExpectEq("touch after slider clamps to last page",
                 settings::SliderTouchXToPageIndex(500, 20, 180, 200), 199);
  test::ExpectEq("touch in middle maps near midpoint",
                 settings::SliderTouchXToPageIndex(110, 20, 180, 200), 99);

  test::ExpectEq("fill width at first page", settings::SliderPageIndexToFillWidth(
                                              0, 200, 180),
                 0);
  test::ExpectEq("fill width at last page", settings::SliderPageIndexToFillWidth(
                                             199, 200, 180),
                 180);
  test::ExpectEq("fill width clamps current page",
                 settings::SliderPageIndexToFillWidth(250, 200, 180), 180);

  return 0;
}
