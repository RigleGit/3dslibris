#include <stdint.h>

#include "reader/reader_controls.h"
#include "test_assert.h"

struct KeyMap {
  uint32_t up = 1u << 0, down = 1u << 1, left = 1u << 2, right = 1u << 3;
  uint32_t dup = 1u << 4, ddown = 1u << 5, dleft = 1u << 6, dright = 1u << 7;
  uint32_t a = 1u << 8, b = 1u << 9, x = 1u << 10, y = 1u << 11;
  uint32_t l = 1u << 12, r = 1u << 13, zl = 1u << 14, zr = 1u << 15;
  uint32_t start = 1u << 16, select = 1u << 17;
  uint32_t downrepeat = 1u << 18;
};

// Golden masks captured from the pre-scheme BuildPortraitControls and
// BuildLandscapeControls implementations (same KeyMap bit layout as above).
struct Golden {
  uint32_t page_next, page_prev, rnd, rpd, rnc, rpc, bn, bp, ln, lp;
  uint32_t fpn, fpp, fcn, fcp, zi, zo, bl, os;
};
static const Golden kGoldenPortrait = {
    0x6122, 0x9211, 0x20, 0x10, 0x2, 0x1, 0x44, 0x88, 0x60, 0x90,
    0x80, 0x40, 0x20, 0x10, 0x100, 0x200, 0x10000, 0x20000};
static const Golden kGoldenLandscape = {
    0x6188, 0x9244, 0x80, 0x40, 0x8, 0x4, 0x22, 0x11, 0x22, 0x11,
    0x80, 0x40, 0x20, 0x10, 0x100, 0x200, 0x10000, 0x20000};

static void CheckGolden(const char *n, const ReaderControls &c, const Golden &g) {
  std::string s(n);
  test::ExpectEqU((s + " page_next").c_str(), c.page_next, g.page_next);
  test::ExpectEqU((s + " page_prev").c_str(), c.page_prev, g.page_prev);
  test::ExpectEqU((s + " repeat next dpad").c_str(), c.page_repeat_next_dpad, g.rnd);
  test::ExpectEqU((s + " repeat prev dpad").c_str(), c.page_repeat_prev_dpad, g.rpd);
  test::ExpectEqU((s + " repeat next circle").c_str(), c.page_repeat_next_circle, g.rnc);
  test::ExpectEqU((s + " repeat prev circle").c_str(), c.page_repeat_prev_circle, g.rpc);
  test::ExpectEqU((s + " bookmark next").c_str(), c.bookmark_next, g.bn);
  test::ExpectEqU((s + " bookmark prev").c_str(), c.bookmark_prev, g.bp);
  test::ExpectEqU((s + " link next").c_str(), c.link_next, g.ln);
  test::ExpectEqU((s + " link prev").c_str(), c.link_prev, g.lp);
  test::ExpectEqU((s + " fixed page next").c_str(), c.fixed_page_next, g.fpn);
  test::ExpectEqU((s + " fixed page prev").c_str(), c.fixed_page_prev, g.fpp);
  test::ExpectEqU((s + " fixed chapter next").c_str(), c.fixed_chapter_next, g.fcn);
  test::ExpectEqU((s + " fixed chapter prev").c_str(), c.fixed_chapter_prev, g.fcp);
  test::ExpectEqU((s + " zoom in").c_str(), c.zoom_in, g.zi);
  test::ExpectEqU((s + " zoom out").c_str(), c.zoom_out, g.zo);
  test::ExpectEqU((s + " back").c_str(), c.back_to_library, g.bl);
  test::ExpectEqU((s + " settings").c_str(), c.open_settings, g.os);
}

static void TestDefaultGolden() {
  KeyMap key;
  CheckGolden("portrait", BuildPortraitControls(key), kGoldenPortrait);
  CheckGolden("landscape", BuildLandscapeControls(key), kGoldenLandscape);
  CheckGolden("scheme portrait",
              BuildReaderControls(CONTROL_SCHEME_DEFAULT, false, key),
              kGoldenPortrait);
  CheckGolden("scheme landscape",
              BuildReaderControls(CONTROL_SCHEME_DEFAULT, true, key),
              kGoldenLandscape);
}

// Previously raw app.key.* uses in the reflow/fixed handlers.
static void TestDefaultFormerRawKeys() {
  KeyMap key;
  for (int land = 0; land < 2; ++land) {
    const ReaderControls c = BuildReaderControls(CONTROL_SCHEME_DEFAULT, land != 0, key);
    test::ExpectEqU("focus_hold", c.focus_hold, key.y);
    test::ExpectEqU("link_activate", c.link_activate, key.a);
    test::ExpectEqU("link_exit", c.link_exit, key.b | key.y);
    test::ExpectEqU("link_follow_image", c.link_follow_image, key.x);
    test::ExpectEqU("color_cycle", c.color_cycle, key.x);
    test::ExpectEqU("fixed_shoulder_next", c.fixed_shoulder_next, key.r | key.zl);
    test::ExpectEqU("fixed_shoulder_prev", c.fixed_shoulder_prev, key.l | key.zr);
  }
}

// App::ApplyRenderOrientation swaps L/R and ZL/ZR in KeyMap; resolved masks
// must follow the swapped physical values.
static void TestHandednessSwap() {
  KeyMap key;
  KeyMap sw = key;
  sw.l = key.r; sw.r = key.l; sw.zl = key.zr; sw.zr = key.zl;
  const ReaderControls c = BuildPortraitControls(sw);
  test::ExpectEqU("swapped page_next", c.page_next,
                  key.a | key.l | key.down | key.ddown | key.zr);
  test::ExpectEqU("swapped shoulder next", c.fixed_shoulder_next, key.l | key.zr);
}

int main() {
  TestDefaultGolden();
  TestDefaultFormerRawKeys();
  TestHandednessSwap();
  return 0;
}
