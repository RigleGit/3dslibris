#include "ui/button_text_layout.h"
#include "test_assert.h"

using namespace button_text_layout;

// Metrics of a typical sans face at `px` (ascent 0.905em, descent 0.212em).
static void Metrics(int px, int *lh, int *asc, int *desc) {
  *asc = (px * 905 + 999) / 1000;
  *desc = (px * 212 + 999) / 1000;
  *lh = (px * 1149 + 500) / 1000;
}

// Settings row heights for PrefsRowPitch 38 / 35 / 32 (pitch - 2).
int main() {
  const int heights[] = {36, 34, 30};
  for (int h = 0; h < 3; h++) {
    const int bh = heights[h];
    for (int lines = 2; lines <= 3; lines++) {
      int px = 12;
      Block b;
      int lh, asc, desc;
      for (;;) {
        Metrics(px, &lh, &asc, &desc);
        b = Compute(bh, lines, lh, asc, desc);
        if (b.fits || px <= 8)
          break;
        px--;
      }
      if (lines == 2) {
        test::ExpectTrue("block fits after shrinking", b.fits);
        const int top_ink = b.first_baseline - InkTop(asc);
        const int bottom_ink = b.first_baseline + (lines - 1) * b.pitch + desc;
        test::ExpectTrue("first line clears the top border", top_ink >= InsetTop(bh));
        test::ExpectTrue("last descender stays above the bottom border",
                         bottom_ink <= bh - InsetBottom(bh));
        test::ExpectTrue("pitch never exceeds the line height", b.pitch <= lh);
        test::ExpectTrue("lines do not collide",
                         b.pitch >= InkTop(asc) + desc);
      }
    }
  }
  // 30px rows cannot hold two 12px lines; the text steps down.
  int lh, asc, desc;
  Metrics(12, &lh, &asc, &desc);
  test::ExpectTrue("12px does not fit 30px rows",
                   !Compute(30, 2, lh, asc, desc).fits);
  Metrics(9, &lh, &asc, &desc);
  test::ExpectTrue("9px fits 30px rows", Compute(30, 2, lh, asc, desc).fits);
  Metrics(11, &lh, &asc, &desc);
  Metrics(12, &lh, &asc, &desc);
  test::ExpectTrue("12px fits 34px rows", Compute(34, 2, lh, asc, desc).fits);
  // A tall row keeps the natural pitch and centers the block.
  Metrics(12, &lh, &asc, &desc);
  Block roomy = Compute(60, 2, lh, asc, desc);
  test::ExpectEq("natural pitch when room", roomy.pitch, lh);
  const int top = roomy.first_baseline - InkTop(asc);
  const int bottom = 60 - (roomy.first_baseline + roomy.pitch + desc);
  const int slack_top = top - InsetTop(60), slack_bottom = bottom - InsetBottom(60);
  test::ExpectTrue("block is centered between the insets",
                   slack_top - slack_bottom <= 1 && slack_bottom - slack_top <= 1);
  // HeightForLines holds any number of lines at the natural pitch.
  for (int px = 9; px <= 14; px++) {
    Metrics(px, &lh, &asc, &desc);
    for (int lines = 2; lines <= 8; lines++) {
      const int h = HeightForLines(lines, lh, asc, desc, 0);
      Block b = Compute(h, lines, lh, asc, desc);
      test::ExpectTrue("grown box fits", b.fits);
      test::ExpectEq("grown box keeps natural pitch", b.pitch, lh);
    }
  }
  test::ExpectTrue("min height is honored",
                   HeightForLines(1, 14, 11, 3, 44) == 44);
  return 0;
}
