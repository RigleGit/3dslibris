#include "app/app.h"
#include "app/main_loop_controller.h"
#include <cstdio>

int g_main_loop_frames = 0, g_main_loop_limit = 6, g_frame_ms = 16;
bool g_cache_idle_scenario = false;

int main() {
  App app;
  MainLoopController loop(app);
  const int result = loop.RunMainLoop();
  bool ok = result == 0 && app.pans == 6 && app.presentations == 6;
  if (app.BackgroundDraws() != 1) {
    std::fprintf(stderr, "FAIL: six pans redrew the gradient %d times; expected one initial draw\n",
                 app.BackgroundDraws());
    ok = false;
  }
  if (app.visible_huds != app.presentations) {
    std::fprintf(stderr, "FAIL: HUD visible in %d/%d presented pan frames\n",
                 app.visible_huds, app.presentations);
    ok = false;
  }
  if (!ok) return 1;
  g_main_loop_frames = 0; g_main_loop_limit = 36; g_frame_ms = 100;
  g_cache_idle_scenario = true;
  App idle;
  idle.simulate_pan = false;
  MainLoopController idle_loop(idle);
  idle_loop.RunMainLoop();
  // Reading never starts or advances EPUB page-cache persistence, even when idle.
  if (idle.GetCurrentBook()->cache_pumps != 0) {
    std::fprintf(stderr, "FAIL: expected no idle cache writes, got %u\n",
                 idle.GetCurrentBook()->cache_pumps);
    return 1;
  }
  std::puts("Main-loop preview pan passed: six pans, one background draw, HUD visible in every frame");
  return 0;
}
