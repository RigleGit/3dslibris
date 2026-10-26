#include "settings/prefs_button_context_utils.h"

namespace settings {

int PrefsRowPitch(unsigned char visible_count) {
  // Eight per-book style rows must clear the footer, including touch slack.
  return visible_count > 7 ? 35 : 38;
}

int PrefsButtonIndexForAction(int action) {
  if (action == PREFS_ACTION_MOBI_LINE_WRAP_FIX ||
      action == PREFS_ACTION_READING_DIRECTION)
    return PREFS_BUTTON_LIBRARY_VIEW;
  if (action == PREFS_ACTION_WIDE_TOP_SCREEN)
    return PREFS_BUTTON_TIME_REMAINING;
  if (action == PREFS_ACTION_GO_TO_PAGE)
    return PREFS_BUTTON_TIME24H;
  return action >= 0 && action < PREFS_BUTTON_COUNT ? action : -1;
}

namespace {

static const int kGeneralPrefsButtons[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_BUTTON_TIME24H,
    PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_COLORMODE,
    PREFS_BUTTON_LIBRARY_VIEW,
    PREFS_BUTTON_LIBRARY_SORT,
};

static const int kGeneralExtraButtons[] = {
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_HANDEDNESS,
    PREFS_BUTTON_REOPEN_LAST_BOOK,
    PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN,
    PREFS_BUTTON_RESET_DEFAULTS,
    PREFS_BUTTON_CLEAR_CACHE,
};

static const int kGeneralStyleButtons[] = {
    PREFS_BUTTON_FONT_CONFIG,
    PREFS_BUTTON_FONTSIZE,
    PREFS_BUTTON_LINE_SPACING,
    PREFS_BUTTON_PARASPACING,
    PREFS_BUTTON_PUBLISHER_TEXT_INDENT,
    PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS,
    PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS,
    PREFS_BUTTON_DISABLE_EBOOK_IMAGES,
};

static const int kBookPrefsButtons[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_BUTTON_TIME24H,
    PREFS_BUTTON_SEARCH,
    PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_BOOK_INFO,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
};

static const int kBookPrefsButtonsWithBookOption[] = {
    PREFS_BUTTON_STYLE_CUSTOMIZATION,
    PREFS_ACTION_MOBI_LINE_WRAP_FIX,
    PREFS_BUTTON_TIME24H,
    PREFS_BUTTON_SEARCH,
    PREFS_BUTTON_TIME_REMAINING,
    PREFS_BUTTON_BOOK_INFO,
    PREFS_BUTTON_INDEX,
    PREFS_BUTTON_BOOKMARKS,
};

static const int kReflowBookPrefsPage2Buttons[] = {
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_HANDEDNESS,
    PREFS_BUTTON_FONTSIZE,
    PREFS_BUTTON_LINE_SPACING,
    PREFS_BUTTON_PARASPACING,
    PREFS_BUTTON_PUBLISHER_TEXT_INDENT,
    PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS,
    PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS,
};

static const int kFixedLayoutBookPrefsPage2Buttons[] = {
    PREFS_BUTTON_ORIENTATION,
    PREFS_BUTTON_HANDEDNESS,
    PREFS_ACTION_READING_DIRECTION,
};

} // namespace

unsigned char PrefsPageButtonCount(const PrefsPageContext &context) {
  if (!context.from_book && context.page == 1) {
    return (unsigned char)(sizeof(kGeneralStyleButtons) /
                           sizeof(kGeneralStyleButtons[0]));
  }
  if (!context.from_book && context.page == 2)
    return ExtraPrefsButtonCount();
  if (context.from_book && context.page == 1)
    return BookPrefsPage2ButtonCount(context.fixed_layout) +
           (context.fixed_layout && context.wide_top_capable ? 1 : 0);
  return VisiblePrefsButtonCount(context.from_book,
                                 context.include_line_wrap_fix) -
         (context.from_book && context.fixed_layout ? 1 : 0);
}

static int PageActionForSlot(const PrefsPageContext &context,
                             unsigned char slot) {
  if (slot >= PrefsPageButtonCount(context)) return -1;
  if (!context.from_book && context.page == 1)
    return kGeneralStyleButtons[slot];
  if (!context.from_book && context.page == 2)
    return ExtraPrefsButtonForSlot(slot);
  if (context.from_book && context.page == 1) {
    if (context.fixed_layout && context.wide_top_capable &&
        slot == BookPrefsPage2ButtonCount(true))
      return PREFS_ACTION_WIDE_TOP_SCREEN;
    return BookPrefsPage2ButtonForSlot(context.fixed_layout, slot);
  }
  if (context.from_book && context.fixed_layout) {
    const unsigned count=VisiblePrefsButtonCount(true,context.include_line_wrap_fix);
    for (unsigned i = 0, visible = 0; i<count; ++i) {
      const int id = PrefsButtonForVisibleSlot(true, context.include_line_wrap_fix, i);
      if (id == PREFS_BUTTON_SEARCH) continue;
      if (visible++ == slot) return id;
    }
    return -1;
  }
  return PrefsButtonForVisibleSlot(context.from_book,
                                   context.include_line_wrap_fix, slot);
}

int PrefsPageButtonForSlot(const PrefsPageContext &context,
                           unsigned char slot) {
  const int action = PageActionForSlot(context, slot);
  if (action == PREFS_BUTTON_TIME24H && context.from_book &&
      context.can_go_to_page)
    return PREFS_ACTION_GO_TO_PAGE;
  return action;
}

unsigned char VisiblePrefsButtonCount(bool from_book,
                                      bool include_line_wrap_fix) {
  if (!from_book)
    return (unsigned char)(sizeof(kGeneralPrefsButtons) / sizeof(kGeneralPrefsButtons[0]));
  if (include_line_wrap_fix)
    return (unsigned char)(sizeof(kBookPrefsButtonsWithBookOption) /
                           sizeof(kBookPrefsButtonsWithBookOption[0]));
  return (unsigned char)(sizeof(kBookPrefsButtons) / sizeof(kBookPrefsButtons[0]));
}

int PrefsButtonForVisibleSlot(bool from_book, bool include_line_wrap_fix,
                              unsigned char slot) {
  if (slot >= VisiblePrefsButtonCount(from_book,include_line_wrap_fix)) return -1;
  if (!from_book)
    return kGeneralPrefsButtons[slot];
  if (include_line_wrap_fix)
    return kBookPrefsButtonsWithBookOption[slot];
  return kBookPrefsButtons[slot];
}

unsigned char ExtraPrefsButtonCount() {
  return (unsigned char)(sizeof(kGeneralExtraButtons) / sizeof(kGeneralExtraButtons[0]));
}

int ExtraPrefsButtonForSlot(unsigned char slot) {
  if (slot >= ExtraPrefsButtonCount()) return -1;
  return kGeneralExtraButtons[slot];
}

unsigned char BookPrefsPage2ButtonCount(bool fixed_layout) {
  if (fixed_layout) {
    return (unsigned char)(sizeof(kFixedLayoutBookPrefsPage2Buttons) /
                           sizeof(kFixedLayoutBookPrefsPage2Buttons[0]));
  }
  return (unsigned char)(sizeof(kReflowBookPrefsPage2Buttons) /
                         sizeof(kReflowBookPrefsPage2Buttons[0]));
}

int BookPrefsPage2ButtonForSlot(bool fixed_layout, unsigned char slot) {
  if (slot >= BookPrefsPage2ButtonCount(fixed_layout)) return -1;
  return fixed_layout ? kFixedLayoutBookPrefsPage2Buttons[slot]
                      : kReflowBookPrefsPage2Buttons[slot];
}

} // namespace settings
