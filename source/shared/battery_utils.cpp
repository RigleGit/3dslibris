#include "shared/battery_utils.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

namespace battery_utils
{

const char *GetApproxPercentLabel(u8 level)
{
  switch (level)
  {
  case 0: return "~1%";
  case 1: return "~20%";
  case 2: return "~40%";
  case 3: return "~60%";
  case 4: return "~80%";
  case 5: return "~100%";
  default: return "?";
  }
}

bool ReadBatteryState(u8 *out_level, bool *out_charging)
{
  if (!out_level || !out_charging)
    return false;

  Result init_result = ptmuInit();
  if (R_FAILED(init_result))
    return false;

  u8 level = 0;
  u8 charge_state = 0;
  Result level_result = PTMU_GetBatteryLevel(&level);
  Result charge_result = 0;
  if (R_SUCCEEDED(level_result))
    charge_result = PTMU_GetBatteryChargeState(&charge_state);

  ptmuExit();

  if (R_FAILED(level_result))
    return false;
  if (R_FAILED(charge_result))
    return false;

  *out_level = level;
  *out_charging = (charge_state != 0);
  return true;
}

void FormatBatteryString(char *buf, size_t bufsz, u8 level, bool charging)
{
  const char *pct = GetApproxPercentLabel(level);
  if (charging)
    snprintf(buf, bufsz, "+%s", pct);
  else
    snprintf(buf, bufsz, "%s", pct);
}

}
