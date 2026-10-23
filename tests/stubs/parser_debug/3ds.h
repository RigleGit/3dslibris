#pragma once
#include "../3ds.h"
#include <mutex>

// Synthetic platform counters for parser diagnostics, not hardware measurements.
#define MEMREGION_ALL 0
#define SYSCLOCK_ARM11 1000000ULL
inline u64 svcGetSystemTick() { return 0; }
inline unsigned osGetMemRegionFree(int) { return 32u * 1024u * 1024u; }
inline unsigned linearSpaceFree() { return 32u * 1024u * 1024u; }
enum APT_HookType {
  APTHOOK_ONSUSPEND, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP,
  APTHOOK_ONWAKEUP, APTHOOK_ONEXIT
};
typedef std::mutex LightLock;
inline void LightLock_Init(LightLock *) {}
inline void LightLock_Lock(LightLock *lock) { lock->lock(); }
inline void LightLock_Unlock(LightLock *lock) { lock->unlock(); }
