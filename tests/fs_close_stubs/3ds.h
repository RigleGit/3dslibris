#pragma once
#include <cstdint>
typedef std::uint32_t u32;
typedef std::uint16_t u16;
typedef u32 Handle;
typedef std::int32_t Result;
#define R_FAILED(result) ((result) < 0)
#define R_SUCCEEDED(result) ((result) >= 0)
u32 *getThreadCommandBuffer();
Result svcSendSyncRequest(Handle handle);
Result svcCloseHandle(Handle handle);
static inline u32 IPC_MakeHeader(u16 command, unsigned normal, unsigned translate) {
  return (u32(command) << 16) | ((normal & 63) << 6) | (translate & 63);
}
