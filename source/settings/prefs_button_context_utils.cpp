#include "settings/prefs_button_context_utils.h"

namespace settings {

namespace {

static const int kGeneralPrefsButtons[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_TIME24H,
  PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_COLORMODE,
    PREFS_BUTTON_LIBRARY_VIEW,
    PREFS_BUTTON_LIBRARY_SORT,
};

static const int kGeneralExtraButtons[] = {
    PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN,
    PREFS_BUTTON_RESET_DEFAULTS,
    PREFS_BUTTON_CLEAR_CACHE,
};

static const int kBookPrefsButtons[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_TIME24H,
  PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
};

static const int kBookPrefsButtonsWithBookOption[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_BUTTON_LIBRARY_VIEW,
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_TIME24H,
  PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
};

} // namespace

unsigned char VisiblePrefsButtonCount(bool from_book, bool include_book_option) {
  if (!from_book)
    return (unsigned char)(sizeof(kGeneralPrefsButtons) / sizeof(kGeneralPrefsButtons[0]));
  if (include_book_option)
    return (unsigned char)(sizeof(kBookPrefsButtonsWithBookOption) /
                           sizeof(kBookPrefsButtonsWithBookOption[0]));
  return (unsigned char)(sizeof(kBookPrefsButtons) / sizeof(kBookPrefsButtons[0]));
}

int PrefsButtonForVisibleSlot(bool from_book, bool include_book_option,
                              unsigned char slot) {
  if (!from_book)
    return kGeneralPrefsButtons[slot];
  if (include_book_option)
    return kBookPrefsButtonsWithBookOption[slot];
  return kBookPrefsButtons[slot];
}

unsigned char ExtraPrefsButtonCount() {
  return (unsigned char)(sizeof(kGeneralExtraButtons) / sizeof(kGeneralExtraButtons[0]));
}

int ExtraPrefsButtonForSlot(unsigned char slot) {
  return kGeneralExtraButtons[slot];
}

} // namespace settings
