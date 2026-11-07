/*
    3dslibris - mcp_input_mailbox.h
    Debug-only live input mailbox for emulator automation (azahar-mcp).

    Summary:
    - Exposes `g_azahar_mcp_input` so a host tool can inject buttons and touch
      through Azahar's RPC WriteMemory without window focus.
    - Layout matches azahar-mcp's `azahar_mcp_mailbox.h` (version 1).
    - Compiled only when DSLIBRIS_DEBUG is defined; release builds are untouched.
*/

#pragma once

#ifdef DSLIBRIS_DEBUG

#include <3ds.h>

extern "C" {

struct AzaharMcpMailbox {
  volatile u32 magic;     // 0x00 "AMCP"
  volatile u32 version;   // 0x04
  volatile u32 seq;       // 0x08 host command sequence (written last)
  volatile u32 ack;       // 0x0C last seq consumed by the app
  volatile u32 keys;      // 0x10 KEY_* mask to hold
  volatile u32 frames;    // 0x14 frames to hold the command
  volatile u32 flags;     // 0x18 bit0: touch active
  volatile u16 touch_x;   // 0x1C
  volatile u16 touch_y;   // 0x1E
  volatile u32 frame;     // 0x20 heartbeat, +1 per main-loop frame
  volatile u32 active;    // 0x24 frames left of the current command
  volatile u32 user[6];   // 0x28 app state export: [0] = AppMode
};

extern AzaharMcpMailbox g_azahar_mcp_input;

}

namespace mcp_input {

// Merge injected input into this frame's HID state (main thread only).
void Apply(u32 &keys_down, u32 &keys_held, touchPosition &touch);
void SetUserValue(unsigned index, u32 value);

} // namespace mcp_input

#endif // DSLIBRIS_DEBUG
