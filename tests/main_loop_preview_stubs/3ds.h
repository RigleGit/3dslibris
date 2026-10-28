#pragma once
#include <cstdint>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int APT_HookType;
struct touchPosition { u16 px, py; };
static const u32 KEY_TOUCH = 1;
extern int g_main_loop_frames, g_main_loop_limit, g_frame_ms;
extern bool g_cache_idle_scenario;
inline bool aptMainLoop() { return g_main_loop_frames++ < g_main_loop_limit; }
inline bool aptIsActive() { return true; }
inline void svcSleepThread(long long) {}
inline void gspWaitForVBlank() {}
inline void hidScanInput() {}
inline void hidTouchRead(touchPosition *touch) { touch->px = 0; touch->py = 0; }
inline u32 hidKeysDown() { return g_cache_idle_scenario &&
    (g_main_loop_frames == 1 || g_main_loop_frames == 15) ? KEY_TOUCH : 0; }
inline u32 hidKeysHeld() { return 0; }
inline u64 osGetTime() { return (u64)g_main_loop_frames * g_frame_ms; }
