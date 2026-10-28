#include <3ds.h>

namespace {

// libctru's FSFILE_Close/FSDIR_Close leave the kernel session open when the
// IPC close fails. newlib nevertheless discards its descriptor, so it cannot
// retry that handle. Own both steps here to release it exactly once on every
// path, including calls made by stdio, FreeType and minizip through libctru.
// Keep this below stdio: logging here could recurse into filesystem cleanup.
static Result CloseFsHandle(Handle handle, u16 command) {
  u32 *cmdbuf = getThreadCommandBuffer();
  cmdbuf[0] = IPC_MakeHeader(command, 0, 0);
  Result result = svcSendSyncRequest(handle);
  if (R_SUCCEEDED(result))
    result = static_cast<Result>(cmdbuf[1]);

  const Result close_result = svcCloseHandle(handle);
  // Preserve write/transport errors even when releasing the session succeeds.
  return R_FAILED(result) ? result : close_result;
}

} // namespace

extern "C" Result __wrap_FSFILE_Close(Handle handle) {
  return CloseFsHandle(handle, 0x808);
}

extern "C" Result __wrap_FSDIR_Close(Handle handle) {
  return CloseFsHandle(handle, 0x802);
}
