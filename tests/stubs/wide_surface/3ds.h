#pragma once
#include <cstdint>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
typedef void *Thread;
struct LightEvent {};
enum { CFG_MODEL_2DS = 3, GFX_TOP = 0, GFX_BOTTOM = 1, GFX_LEFT = 0 };
extern int test_model;
extern Result test_cfg_result;
extern bool test_wide;
extern u8 test_top_buffers[2][240 * 800 * 3];
extern u8 test_bottom_buffers[2][240 * 320 * 3];
extern int test_frame_slot;
inline bool R_FAILED(Result result) { return result < 0; }
inline Result cfguInit() { return test_cfg_result; }
inline Result CFGU_GetSystemModel(u8 *model) {
  *model = (u8)test_model;
  return test_cfg_result;
}
inline void cfguExit() {}
inline void gfxSetWide(bool wide) { test_wide = wide; }
inline u8 *gfxGetFramebuffer(int screen, int, u16 *width, u16 *height) {
  *width = 240;
  *height = screen == GFX_TOP ? (test_wide ? 800 : 400) : 320;
  return screen == GFX_TOP ? test_top_buffers[test_frame_slot]
                           : test_bottom_buffers[test_frame_slot];
}
