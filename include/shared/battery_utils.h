#pragma once

#include <3ds.h>

namespace battery_utils
{

// PTMU level 0-5; returns "?" on read failure.
const char *GetApproxPercentLabel(u8 level);

// Returns false if PTMU read fails. Sets out_level (0-5) and out_charging.
bool ReadBatteryState(u8 *out_level, bool *out_charging);

// buf must be >= 10 bytes. Format: "~XX%" or "+~XX%" when charging.
void FormatBatteryString(char *buf, size_t bufsz, u8 level, bool charging);

} // namespace battery_utils
