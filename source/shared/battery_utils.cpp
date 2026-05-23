#include "shared/battery_utils.h"

#include <3ds.h>
#include <3ds/services/mcuhwc.h>
#include <stdio.h>
#include <string.h>

namespace battery_utils
{

static bool ReadChargingState(bool *out_charging)
{
  if (!out_charging)
    return false;

  Result init_result = ptmuInit();
  if (R_FAILED(init_result))
    return false;

  u8 charge_state = 0;
  Result charge_result = PTMU_GetBatteryChargeState(&charge_state);
  ptmuExit();

  if (R_FAILED(charge_result))
    return false;

  *out_charging = (charge_state != 0);
  return true;
}

static bool ReadPtmApproxPercent(u8 *out_percent)
{
  if (!out_percent)
    return false;

  Result init_result = ptmuInit();
  if (R_FAILED(init_result))
    return false;

  u8 level = 0;
  Result level_result = PTMU_GetBatteryLevel(&level);
  ptmuExit();

  if (R_FAILED(level_result))
    return false;

  switch (level)
  {
  case 0: *out_percent = 1; break;
  case 1: *out_percent = 3; break;
  case 2: *out_percent = 8; break;
  case 3: *out_percent = 20; break;
  case 4: *out_percent = 45; break;
  case 5: *out_percent = 80; break;
  default: return false;
  }
  return true;
}

bool ReadBatteryState(u8 *out_percent, bool *out_charging, bool *out_approx)
{
  if (!out_percent || !out_charging || !out_approx)
    return false;

  bool charging = false;
  if (!ReadChargingState(&charging))
    return false;

  u8 percent = 0;
  bool approx = false;
  Result mcu_result = mcuHwcInit();
  if (R_SUCCEEDED(mcu_result))
  {
    Result level_result = MCUHWC_GetBatteryLevel(&percent);
    mcuHwcExit();
    if (R_FAILED(level_result))
    {
      if (!ReadPtmApproxPercent(&percent))
        return false;
      approx = true;
    }
  }
  else
  {
    if (!ReadPtmApproxPercent(&percent))
      return false;
    approx = true;
  }

  if (percent > 100)
    percent = 100;

  *out_percent = percent;
  *out_charging = charging;
  *out_approx = approx;
  return true;
}

void FormatBatteryString(char *buf, size_t bufsz, u8 percent, bool charging,
                         bool approx)
{
  const char *prefix = charging ? "+" : "";
  const char *approx_prefix = approx ? "~" : "";
  if (charging)
    snprintf(buf, bufsz, "%s%s%u%%", prefix, approx_prefix, percent);
  else
    snprintf(buf, bufsz, "%s%u%%", approx_prefix, percent);
}

}
