#include "ui/button.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

const u16 kBackground = 0x1234;
void CheckOutside(const std::vector<u16> &pixels, int width, int height) {
  for (int y = 0; y < 400; ++y)
    for (int x = 0; x < 400; ++x)
      if (x >= width || y >= height)
        assert(pixels[y * 400 + x] == kBackground);
}
int main() {
  Text ts;
  std::vector<u16> pixels(400 * 400, kBackground);
  // Actual Button -> skin framebuffer writes, not a recording skin mock.
  for (int theme : {0, 1, 2})
    for (bool landscape : {false, true})
      for (bool top : {false, true}) {
        ts.landscape = landscape;
        ts.screen = pixels.data();
        ts.screenleft = top ? pixels.data() : ts.left;
        ts.screenright = top ? ts.right : pixels.data();
        const int width = landscape ? (top ? 400 : 320) : 240;
        const int height = landscape ? 240 : (top ? 400 : 320);
        UiButtonSkin_SetColorMode(theme);
        Button button(&ts);
        button.SetIcon(UI_BUTTON_ICON_NONE);
        button.Move(width - 55, 20);
        button.Resize(50, 44);
        button.Label("fit");
        button.Draw(false);
        assert(pixels[40 * 400 + width - 25] != kBackground);
        CheckOutside(pixels, width, height);
        std::fill(pixels.begin(), pixels.end(), kBackground);
        // Draw partly beyond the edge: adjacent buffer columns/rows are
        // canaries.
        button.Move(width - 20, height - 20);
        button.Draw(false);
        CheckOutside(pixels, width, height);
        std::fill(pixels.begin(), pixels.end(), kBackground);
        for (UiButtonIconId icon :
             {UI_BUTTON_ICON_GEAR, UI_BUTTON_ICON_BACK, UI_BUTTON_ICON_HOME}) {
          UiButtonSkin_DrawIcon(pixels.data(), 400, width, height, width - 55,
                                20, 50, 44, icon, false);
          bool painted = false;
          for (int y = 20; y < 64; ++y)
            for (int x = width - 35; x < width; ++x)
              painted |= pixels[y * 400 + x] != kBackground;
          assert(painted);
          CheckOutside(pixels, width, height);
          std::fill(pixels.begin(), pixels.end(), kBackground);
          UiButtonSkin_DrawIcon(pixels.data(), 400, width, height, width - 5,
                                height - 20, 50, 44, icon, false);
          CheckOutside(pixels, width, height);
          std::fill(pixels.begin(), pixels.end(), kBackground);
        }
      }
  UiButtonSkin_Exit();
  puts(
      "PASS: actual button/icon pixels across portrait/landscape screen edges");
}
