#include "settings/prefs_action_utils.h"

namespace settings {

bool ToggleSetting(bool current) { return !current; }

int NextCyclicSetting(int current, int count) {
  if (count <= 0 || current < 0 || current >= count - 1)
    return 0;
  return current + 1;
}

int NextTriStateOverride(int current) {
  if (current < 0)
    return 0;
  if (current == 0)
    return 1;
  return -1;
}

int NextTextAlignMode(int current, bool allow_inherit) {
  if (current < 0 || current > 2)
    return 0;
  if (current == 2)
    return allow_inherit ? -1 : 0;
  return current + 1;
}

} // namespace settings
