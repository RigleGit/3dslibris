#pragma once
#include <cstdint>
extern uint64_t fake_tick;
extern unsigned fake_region_free;
extern unsigned fake_linear_free;
inline uint64_t svcGetSystemTick() {
  return fake_tick;
}
#define SYSCLOCK_ARM11 1000000ULL
#define MEMREGION_ALL 0
inline unsigned osGetMemRegionFree(int) {
  return fake_region_free;
}
inline unsigned linearSpaceFree() {
  return fake_linear_free;
}

#include <mutex>
typedef std::mutex LightLock;
inline void LightLock_Init(LightLock *) {}
inline void LightLock_Lock(LightLock *l) {
  l->lock();
}
inline void LightLock_Unlock(LightLock *l) {
  l->unlock();
}
