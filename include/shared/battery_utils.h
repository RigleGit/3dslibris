#pragma once

#include <3ds.h>

namespace battery_utils
{

// Returns false if battery state cannot be read.
// Prefer MCUHWC exact percentage; fall back to PTMU coarse percentage.
bool ReadBatteryState(u8 *out_percent, bool *out_charging, bool *out_approx);

// buf must be >= 10 bytes. Format: "XX%" or "+XX%" when charging.
void FormatBatteryString(char *buf, size_t bufsz, u8 percent, bool charging,
                         bool approx);

} // namespace battery_utils
