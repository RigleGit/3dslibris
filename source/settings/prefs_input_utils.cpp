#include "settings/prefs_input_utils.h"

namespace prefs_input_utils {

bool ShouldReturnFromPrefs(uint32_t keys, bool book_context,
                           uint32_t b_key, uint32_t select_key,
                           uint32_t y_key, uint32_t start_key) {
  if (book_context)
    return (keys & (b_key | select_key | y_key)) != 0;
  return (keys & (b_key | select_key | y_key | start_key)) != 0;
}

bool ShouldRedrawPrefsAfterOverlayInput(bool prefs_dirty,
                                        bool prefs_mode_active) {
  return prefs_dirty && prefs_mode_active;
}

PrefsNavStep StepPrefsSelection(int page, int selected, int visible_count,
                                bool book_ctx, bool has_submenu, int direction,
                                int book_page_count) {
  PrefsNavStep step;
  step.page = page;
  step.selected = selected;
  step.select_last = false;
  step.changed = false;
  if (direction > 0) {
    if (selected < visible_count - 1) {
      step.selected = selected + 1;
      step.changed = true;
    } else if (!book_ctx && page == 0) {
      step.page = 2;
      step.selected = 0;
      step.changed = true;
    } else if (!book_ctx && page == 2) {
      step.page = 3;
      step.selected = 0;
      step.changed = true;
    } else if (book_ctx && page == 0 && has_submenu) {
      step.page = 1;
      step.selected = 0;
      step.changed = true;
    } else if (book_ctx && page == 1 && book_page_count > 2) {
      step.page = 2;
      step.selected = 0;
      step.changed = true;
    }
  } else {
    if (selected > 0) {
      step.selected = selected - 1;
      step.changed = true;
    } else if (!book_ctx && page == 3) {
      step.page = 2;
      step.select_last = true;
      step.changed = true;
    } else if (!book_ctx && page == 2) {
      step.page = 0;
      step.select_last = true;
      step.changed = true;
    } else if (book_ctx && page == 2 && book_page_count > 2) {
      step.page = 1;
      step.select_last = true;
      step.changed = true;
    } else if (page == 1 && has_submenu) {
      // Page 1 is reachable from both contexts (book submenu, or the
      // library "style customization" button).
      step.page = 0;
      step.select_last = true;
      step.changed = true;
    }
  }
  return step;
}

} // namespace prefs_input_utils
