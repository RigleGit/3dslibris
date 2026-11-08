#include "settings/prefs_input_utils.h"

#include "test_assert.h"

using prefs_input_utils::PrefsNavStep;
using prefs_input_utils::StepPrefsSelection;

static void TestNextInsidePageMovesSelection() {
  PrefsNavStep s = StepPrefsSelection(0, 1, 5, false, true, +1);
  test::ExpectTrue("next inside page changed", s.changed);
  test::ExpectEq("next inside page keeps page", s.page, 0);
  test::ExpectEq("next inside page moves selection", s.selected, 2);
  test::ExpectFalse("next inside page no select_last", s.select_last);
}

static void TestNextAtLastSlotRollsToNextPage() {
  PrefsNavStep lib = StepPrefsSelection(0, 4, 5, false, true, +1);
  test::ExpectTrue("library roll changed", lib.changed);
  test::ExpectEq("library roll page", lib.page, 2);
  test::ExpectEq("library roll selected", lib.selected, 0);
  test::ExpectFalse("library roll select_last", lib.select_last);
  PrefsNavStep book = StepPrefsSelection(0, 4, 5, true, true, +1);
  test::ExpectTrue("book roll changed", book.changed);
  test::ExpectEq("book roll page", book.page, 1);
  test::ExpectEq("book roll selected", book.selected, 0);
}

static void TestNextAtLastSlotWithoutSubmenuStays() {
  PrefsNavStep s = StepPrefsSelection(0, 4, 5, true, false, +1);
  test::ExpectFalse("no submenu stays", s.changed);
  test::ExpectEq("no submenu page", s.page, 0);
  test::ExpectEq("no submenu selected", s.selected, 4);
}

static void TestPrevAtFirstSlotLandsOnLastSlot() {
  PrefsNavStep lib = StepPrefsSelection(2, 0, 3, false, true, -1);
  test::ExpectTrue("library back changed", lib.changed);
  test::ExpectEq("library back page", lib.page, 0);
  test::ExpectTrue("library back select_last", lib.select_last);
  PrefsNavStep book = StepPrefsSelection(1, 0, 3, true, true, -1);
  test::ExpectTrue("book back changed", book.changed);
  test::ExpectEq("book back page", book.page, 0);
  test::ExpectTrue("book back select_last", book.select_last);
}

static void TestLibraryStyleSubmenuBackReturnsToPageZero() {
  // Library context reaches page 1 through the "style customization" button.
  PrefsNavStep s = StepPrefsSelection(1, 0, 3, false, true, -1);
  test::ExpectTrue("library style back changed", s.changed);
  test::ExpectEq("library style back page", s.page, 0);
  test::ExpectTrue("library style back select_last", s.select_last);
}

static void TestPrevInsidePageMovesSelection() {
  PrefsNavStep s = StepPrefsSelection(0, 3, 5, false, true, -1);
  test::ExpectTrue("prev inside changed", s.changed);
  test::ExpectEq("prev inside selected", s.selected, 2);
  test::ExpectFalse("prev inside select_last", s.select_last);
}

static void TestPrevAtFirstSlotOfPageZeroStays() {
  PrefsNavStep s = StepPrefsSelection(0, 0, 5, false, true, -1);
  test::ExpectFalse("page 0 slot 0 stays", s.changed);
  test::ExpectEq("page 0 slot 0 selected", s.selected, 0);
}

static void TestNextAtLastSlotOfFinalPageStays() {
  PrefsNavStep s = StepPrefsSelection(2, 2, 3, false, true, +1);
  test::ExpectFalse("final page last slot stays", s.changed);
  test::ExpectEq("final page selected", s.selected, 2);
}

int main() {
  TestNextInsidePageMovesSelection();
  TestNextAtLastSlotRollsToNextPage();
  TestNextAtLastSlotWithoutSubmenuStays();
  TestPrevAtFirstSlotLandsOnLastSlot();
  TestLibraryStyleSubmenuBackReturnsToPageZero();
  TestPrevInsidePageMovesSelection();
  TestPrevAtFirstSlotOfPageZeroStays();
  TestNextAtLastSlotOfFinalPageStays();
  return 0;
}
