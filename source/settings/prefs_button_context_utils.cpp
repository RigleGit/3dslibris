#include "settings/prefs_button_context_utils.h"

namespace settings {

namespace {

static const int kGeneralPrefsButtons[] = {
    PREFS_BUTTON_FONT_CONFIG, PREFS_BUTTON_FONTSIZE,    PREFS_BUTTON_PARASPACING,
    PREFS_BUTTON_ORIENTATION, PREFS_BUTTON_TIME24H,     PREFS_BUTTON_COLORMODE,
    PREFS_BUTTON_LIBRARY_VIEW,
};

static const int kBookPrefsButtons[] = {
    PREFS_BUTTON_FONT_CONFIG,
    PREFS_BUTTON_FONTSIZE,
    PREFS_BUTTON_PARASPACING,
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
};

static const int kBookPrefsButtonsWithLineWrapFix[] = {
    PREFS_BUTTON_FONT_CONFIG,
    PREFS_BUTTON_FONTSIZE,
    PREFS_BUTTON_PARASPACING,
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
    PREFS_BUTTON_LIBRARY_VIEW,
};

} // namespace

unsigned char VisiblePrefsButtonCount(bool from_book, bool include_book_option) {
  if (!from_book)
    return (unsigned char)(sizeof(kGeneralPrefsButtons) / sizeof(kGeneralPrefsButtons[0]));
  if (include_book_option) {
    return (unsigned char)(sizeof(kBookPrefsButtonsWithLineWrapFix) /
                           sizeof(kBookPrefsButtonsWithLineWrapFix[0]));
  }
  return (unsigned char)(sizeof(kBookPrefsButtons) / sizeof(kBookPrefsButtons[0]));
}

int PrefsButtonForVisibleSlot(bool from_book, bool include_book_option,
                              unsigned char slot) {
  if (!from_book)
    return kGeneralPrefsButtons[slot];
  if (include_book_option)
    return kBookPrefsButtonsWithLineWrapFix[slot];
  return kBookPrefsButtons[slot];
}

} // namespace settings
