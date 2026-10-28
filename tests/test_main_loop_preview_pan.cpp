#include "app/app.h"
#include "app/main_loop_controller.h"
#include <cstdio>

int g_main_loop_frames = 0;

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
  std::puts("Main-loop preview pan passed: six pans, one background draw, HUD visible in every frame");
  return 0;
}
