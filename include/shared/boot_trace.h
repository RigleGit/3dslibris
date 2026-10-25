#pragma once

#include <3ds.h>

namespace boot_trace {

#ifdef DSLIBRIS_DEBUG
// Main-thread only. APT hooks enqueue the event time without filesystem I/O.
// Pending events are lost if HOME crashes before FlushAptHooks can run.
void Boot(const char *message);
void RecordAptHook(APT_HookType hook);
void FlushAptHooks();
#else
inline void Boot(const char *) {}
inline void RecordAptHook(APT_HookType) {}
inline void FlushAptHooks() {}
#endif

} // namespace boot_trace
