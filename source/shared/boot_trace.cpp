#include "shared/boot_trace.h"

#include <atomic>
#include <stdio.h>

namespace {

const int kPendingAptHookCapacity = 16;
std::atomic<int> g_pending_apt_hook_write_index(0);
std::atomic<int> g_pending_apt_hook_flush_index(0);
std::atomic<int> g_pending_apt_hooks[kPendingAptHookCapacity];

void AppendTraceLine(const char *path, const char *message) {
  if (!path || !message)
    return;
  FILE *fp = fopen(path, "a");
  if (!fp)
    return;
  fprintf(fp, "%llu %s\n", (unsigned long long)osGetTime(), message);
  fflush(fp);
  fclose(fp);
}

const char *AptHookName(APT_HookType hook) {
  switch (hook) {
  case APTHOOK_ONSUSPEND:
    return "APTHOOK_ONSUSPEND";
  case APTHOOK_ONRESTORE:
    return "APTHOOK_ONRESTORE";
  case APTHOOK_ONSLEEP:
    return "APTHOOK_ONSLEEP";
  case APTHOOK_ONWAKEUP:
    return "APTHOOK_ONWAKEUP";
  case APTHOOK_ONEXIT:
    return "APTHOOK_ONEXIT";
  default:
    return "APTHOOK_UNKNOWN";
  }
}

} // namespace

namespace boot_trace {

void Boot(const char *message) {
  AppendTraceLine("sdmc:/3dslibris_boot_trace.txt", message);
}

void RecordAptHook(APT_HookType hook) {
  const int index =
      g_pending_apt_hook_write_index.fetch_add(1, std::memory_order_relaxed);
  g_pending_apt_hooks[index % kPendingAptHookCapacity].store(
      (int)hook, std::memory_order_relaxed);
}

void FlushAptHooks() {
  int flush_index = g_pending_apt_hook_flush_index.load(std::memory_order_relaxed);
  const int write_index =
      g_pending_apt_hook_write_index.load(std::memory_order_relaxed);
  if (write_index - flush_index > kPendingAptHookCapacity)
    flush_index = write_index - kPendingAptHookCapacity;

  while (flush_index < write_index) {
    const int hook_value =
        g_pending_apt_hooks[flush_index % kPendingAptHookCapacity].load(
            std::memory_order_relaxed);
    const APT_HookType hook = (APT_HookType)hook_value;
    char line[64];
    snprintf(line, sizeof(line), "%s value=%d", AptHookName(hook), hook_value);
    AppendTraceLine("sdmc:/3dslibris_apt_trace.txt", line);
    flush_index++;
  }
  g_pending_apt_hook_flush_index.store(flush_index, std::memory_order_relaxed);
}

} // namespace boot_trace
