#pragma once

#include <cstdint>

typedef std::uint64_t u64;
enum APT_HookType {
  APTHOOK_ONSUSPEND,
  APTHOOK_ONRESTORE,
  APTHOOK_ONSLEEP,
  APTHOOK_ONWAKEUP,
  APTHOOK_ONEXIT
};

extern u64 boot_trace_test_time;
inline u64 osGetTime() { return boot_trace_test_time; }
