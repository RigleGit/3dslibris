#include <3ds.h>
#include <cassert>
#include <cstdio>

extern "C" Result __wrap_FSFILE_Close(Handle);
extern "C" Result __wrap_FSDIR_Close(Handle);

static u32 command_buffer[2];
static Result transport_result, service_result, kernel_result;
static unsigned expected_command;
static int ipc_calls, kernel_calls;
static bool handle_alive;

u32 *getThreadCommandBuffer() { return command_buffer; }
Result svcSendSyncRequest(Handle handle) {
  assert(handle == 42 && handle_alive);
  assert(command_buffer[0] == expected_command);
  assert(kernel_calls == 0); // Send the FS close before releasing the session.
  ++ipc_calls;
  command_buffer[1] = static_cast<u32>(service_result);
  return transport_result;
}
Result svcCloseHandle(Handle handle) {
  assert(handle == 42 && handle_alive && ipc_calls == 1);
  ++kernel_calls;
  if (R_SUCCEEDED(kernel_result))
    handle_alive = false;
  return kernel_result;
}

int main() {
  typedef Result (*Close)(Handle);
  const Close closers[] = {__wrap_FSFILE_Close, __wrap_FSDIR_Close};
  const unsigned commands[] = {0x08080000, 0x08020000};
  const Result errors[][3] = {
      {0, 0, 0}, {-1, 0, 0}, {0, -2, 0}, {0, 0, -3}, {-1, 0, -3}, {0, -2, -3}};
  for (unsigned owner = 0; owner < 2; ++owner) {
    expected_command = commands[owner];
    for (unsigned fault = 0; fault < 6; ++fault) {
      transport_result = errors[fault][0];
      service_result = errors[fault][1];
      kernel_result = errors[fault][2];
      handle_alive = true;
      ipc_calls = kernel_calls = 0;
      const Result expected = transport_result < 0 ? transport_result :
          (service_result < 0 ? service_result : kernel_result);
      assert(closers[owner](42) == expected);
      assert(ipc_calls == 1 && kernel_calls == 1);
      assert(handle_alive == (kernel_result < 0));
    }
  }
  puts("FS close releases each session once, preserving IPC and kernel errors");
}
