#include "settings/prefs_button_context_utils.h"

#include "test_assert.h"

int main() {
  test::ExpectEq("general visible count",
                 settings::VisiblePrefsButtonCount(false, false), 6);
  test::ExpectEq("book visible count without line wrap",
                 settings::VisiblePrefsButtonCount(true, false), 5);
  test::ExpectEq("book visible count with line wrap",
                 settings::VisiblePrefsButtonCount(true, true), 6);

  test::ExpectEq("general slot 0", settings::PrefsButtonForVisibleSlot(false, false, 0),
                 PREFS_BUTTON_STYLE_CUSTOMIZATION);
  test::ExpectEq("general slot 2", settings::PrefsButtonForVisibleSlot(false, false, 2),
                 PREFS_BUTTON_TIME24H);
  test::ExpectEq("general slot 3", settings::PrefsButtonForVisibleSlot(false, false, 3),
                 PREFS_BUTTON_COLORMODE);
  test::ExpectEq("general slot 5", settings::PrefsButtonForVisibleSlot(false, false, 5),
                 PREFS_BUTTON_LIBRARY_SORT);

  test::ExpectEq("extra button count", settings::ExtraPrefsButtonCount(), 3);
  test::ExpectEq("extra slot 0", settings::ExtraPrefsButtonForSlot(0),
                 PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN);
  test::ExpectEq("extra slot 1", settings::ExtraPrefsButtonForSlot(1),
                 PREFS_BUTTON_RESET_DEFAULTS);
  test::ExpectEq("extra slot 2", settings::ExtraPrefsButtonForSlot(2),
                 PREFS_BUTTON_CLEAR_CACHE);

  test::ExpectEq("book slot 0", settings::PrefsButtonForVisibleSlot(true, false, 0),
                 PREFS_BUTTON_STYLE_CUSTOMIZATION);
  test::ExpectEq("book slot 1", settings::PrefsButtonForVisibleSlot(true, false, 1),
                 PREFS_BUTTON_ORIENTATION);
  test::ExpectEq("book slot 2", settings::PrefsButtonForVisibleSlot(true, false, 2),
                 PREFS_BUTTON_TIME24H);
  test::ExpectEq("book slot 3", settings::PrefsButtonForVisibleSlot(true, false, 3),
                 PREFS_BUTTON_INDEX);
  test::ExpectEq("book slot 4", settings::PrefsButtonForVisibleSlot(true, false, 4),
                 PREFS_BUTTON_BOOKMARKS);

  test::ExpectEq("book slot 0 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 0),
                 PREFS_BUTTON_STYLE_CUSTOMIZATION);
  test::ExpectEq("book slot 1 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 1),
                 PREFS_BUTTON_LIBRARY_VIEW);
  test::ExpectEq("book slot 2 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 2),
                 PREFS_BUTTON_ORIENTATION);
  test::ExpectEq("book slot 3 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 3),
                 PREFS_BUTTON_TIME24H);
  test::ExpectEq("book slot 4 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 4),
                 PREFS_BUTTON_INDEX);
  test::ExpectEq("book slot 5 line wrap", settings::PrefsButtonForVisibleSlot(true, true, 5),
                 PREFS_BUTTON_BOOKMARKS);

  for (unsigned char slot = 0; slot < settings::VisiblePrefsButtonCount(true, false);
       slot++) {
    test::ExpectNe("book settings exclude Circle Pad setting",
                   settings::PrefsButtonForVisibleSlot(true, false, slot),
                   PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN);
  }
  for (unsigned char slot = 0; slot < settings::VisiblePrefsButtonCount(true, true);
       slot++) {
    test::ExpectNe("book settings with line wrap exclude Circle Pad setting",
                   settings::PrefsButtonForVisibleSlot(true, true, slot),
                   PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN);
  }

  return 0;
}
