#include "settings/prefs_button_context_utils.h"

#include "test_assert.h"

int main() {
  test::ExpectEq("general visible count",
                 settings::VisiblePrefsButtonCount(false, false), 7);
  test::ExpectEq("book visible count without line wrap",
                 settings::VisiblePrefsButtonCount(true, false), 3);
  test::ExpectEq("book visible count with line wrap",
                 settings::VisiblePrefsButtonCount(true, true), 4);

  test::ExpectEq("general slot 0", settings::PrefsButtonForVisibleSlot(false, false, 0),
                 PREFS_BUTTON_FONT_CONFIG);
  test::ExpectEq("general slot 4", settings::PrefsButtonForVisibleSlot(false, false, 4),
                 PREFS_BUTTON_TIME24H);
  test::ExpectEq("general slot 5", settings::PrefsButtonForVisibleSlot(false, false, 5),
                 PREFS_BUTTON_COLORMODE);
  test::ExpectEq("general slot 6", settings::PrefsButtonForVisibleSlot(false, false, 6),
                 PREFS_BUTTON_LIBRARY_VIEW);

  test::ExpectEq("book slot 0", settings::PrefsButtonForVisibleSlot(true, false, 0),
                 PREFS_BUTTON_TIME24H);
  test::ExpectEq("book slot 1", settings::PrefsButtonForVisibleSlot(true, false, 1),
                 PREFS_BUTTON_INDEX);
  test::ExpectEq("book slot 2", settings::PrefsButtonForVisibleSlot(true, false, 2),
                 PREFS_BUTTON_BOOKMARKS);
  test::ExpectEq("book slot 3 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 3),
                 PREFS_BUTTON_LIBRARY_VIEW);

  return 0;
}
