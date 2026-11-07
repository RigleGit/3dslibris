/*
    3dslibris - mcp_input_mailbox.cpp
    Debug-only live input mailbox for emulator automation (azahar-mcp).
    See include/shared/mcp_input_mailbox.h for the protocol summary.
*/

#include "shared/mcp_input_mailbox.h"

#ifdef DSLIBRIS_DEBUG

extern "C" {
AzaharMcpMailbox g_azahar_mcp_input __attribute__((used)) = {
    0x50434D41u, 1u, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0}};
}

namespace mcp_input {

namespace {
u32 prev_injected = 0;
u32 cur_keys = 0;
u32 cur_flags = 0;
u16 cur_x = 0;
u16 cur_y = 0;
} // namespace

void Apply(u32 &keys_down, u32 &keys_held, touchPosition &touch)
{
  AzaharMcpMailbox &mb = g_azahar_mcp_input;
  mb.frame = mb.frame + 1;
  if (mb.seq != mb.ack)
  {
    cur_keys = mb.keys;
    cur_flags = mb.flags;
    cur_x = mb.touch_x;
    cur_y = mb.touch_y;
    mb.active = mb.frames ? mb.frames : 1;
    mb.ack = mb.seq;
  }
  u32 injected = 0;
  if (mb.active)
  {
    injected = cur_keys;
    if (cur_flags & 1u)
    {
      injected |= KEY_TOUCH;
      touch.px = cur_x;
      touch.py = cur_y;
    }
    mb.active = mb.active - 1;
  }
  keys_held |= injected;
  keys_down |= injected & ~prev_injected;
  prev_injected = injected;
}

void SetUserValue(unsigned index, u32 value)
{
  if (index < 6)
    g_azahar_mcp_input.user[index] = value;
}

} // namespace mcp_input

#endif // DSLIBRIS_DEBUG
