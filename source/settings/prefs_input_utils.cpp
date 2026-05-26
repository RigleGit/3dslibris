#include "settings/prefs_input_utils.h"

namespace prefs_input_utils {

bool ShouldReturnFromPrefs(uint32_t keys, bool book_context,
                           uint32_t b_key, uint32_t select_key,
                           uint32_t y_key, uint32_t start_key) {
  if (book_context)
    return (keys & (b_key | select_key | y_key)) != 0;
  return (keys & (b_key | select_key | y_key | start_key)) != 0;
}

} // namespace prefs_input_utils
