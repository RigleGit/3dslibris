#pragma once

namespace settings {

bool ToggleSetting(bool current);
int NextCyclicSetting(int current, int count);
int NextTriStateOverride(int current);
// Text alignment: 0 editorial, 1 left, 2 justify. A book may also inherit (-1).
int NextTextAlignMode(int current, bool allow_inherit);

} // namespace settings
