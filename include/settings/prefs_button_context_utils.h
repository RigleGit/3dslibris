#pragma once

#include "settings/prefs_button_ids.h"

namespace settings {

struct PrefsPageContext {
  bool from_book;
  int page;
  bool fixed_layout;
  bool include_line_wrap_fix;
  bool can_go_to_page;

  PrefsPageContext()
      : from_book(false), page(0), fixed_layout(false),
        include_line_wrap_fix(false), can_go_to_page(false) {}
};

int PrefsRowPitch(unsigned char visible_count);
int PrefsButtonIndexForAction(int action);

unsigned char PrefsPageButtonCount(const PrefsPageContext &context);
int PrefsPageButtonForSlot(const PrefsPageContext &context,
                           unsigned char slot);

unsigned char VisiblePrefsButtonCount(bool from_book,
                                      bool include_line_wrap_fix);
int PrefsButtonForVisibleSlot(bool from_book, bool include_line_wrap_fix,
                              unsigned char slot);
unsigned char ExtraPrefsButtonCount();
int ExtraPrefsButtonForSlot(unsigned char slot);
unsigned char BookPrefsPage2ButtonCount(bool fixed_layout);
int BookPrefsPage2ButtonForSlot(bool fixed_layout, unsigned char slot);

} // namespace settings
