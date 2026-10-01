#pragma once
// Minimal libctru boundary for the synchronous CBZ integration. Any attempt
// to start or wait on a platform worker fails instead of silently succeeding.
#include "3ds.h"
#include <cassert>
#include <cstddef>
typedef int Result;
typedef int s32;
static const int RESET_STICKY=0, CUR_THREAD_HANDLE=0;
#define R_SUCCEEDED(value) ((value) >= 0)
inline void APT_CheckNew3DS(bool *value) { *value=false; }
inline void LightEvent_Init(LightEvent *, int) { assert(false); }
inline void LightEvent_Signal(LightEvent *) { assert(false); }
inline void LightEvent_Wait(LightEvent *) { assert(false); }
inline void LightEvent_Clear(LightEvent *) { assert(false); }
inline Result svcGetThreadPriority(s32 *, int) { assert(false); return -1; }
inline Thread threadCreate(void (*)(void *), void *, size_t, s32, int, bool) {
  assert(false); return nullptr;
}
inline Result threadJoin(Thread, u64) { assert(false); return -1; }
inline void threadFree(Thread) { assert(false); }
