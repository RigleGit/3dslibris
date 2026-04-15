#pragma once

#include "settings/prefs_button_ids.h"

namespace settings {

unsigned char VisiblePrefsButtonCount(bool from_book, bool include_line_wrap_fix);
int PrefsButtonForVisibleSlot(bool from_book, bool include_line_wrap_fix,
                              unsigned char slot);

} // namespace settings
