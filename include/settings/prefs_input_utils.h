#pragma once

#include <stdint.h>

namespace prefs_input_utils {

bool ShouldReturnFromPrefs(uint32_t keys, bool book_context,
                           uint32_t b_key, uint32_t select_key,
                           uint32_t y_key, uint32_t start_key);

bool ShouldRedrawPrefsAfterOverlayInput(bool prefs_dirty,
                                        bool prefs_mode_active);

struct PrefsNavStep {
  int page;
  int selected;
  bool select_last;  // after switching page, select the new page's last slot
  bool changed;
};

// Moves the selection one slot along the paging axis, rolling over to the
// neighbouring settings page at list ends. direction: +1 next, -1 previous.
// Page graph: library context 0 <-> 2; book context 0 <-> 1 (only when the
// book has a submenu page). Page 1 always steps back to page 0, since the
// library "style customization" button also opens it. A book with three
// pages (book_page_count 3) continues 0 <-> 1 <-> 2.
PrefsNavStep StepPrefsSelection(int page, int selected, int visible_count,
                                bool book_ctx, bool has_submenu, int direction,
                                int book_page_count = 2);

} // namespace prefs_input_utils
