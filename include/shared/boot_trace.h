#pragma once

#include <3ds.h>

namespace boot_trace {

void Boot(const char *message);
void RecordAptHook(APT_HookType hook);
void FlushAptHooks();

} // namespace boot_trace
