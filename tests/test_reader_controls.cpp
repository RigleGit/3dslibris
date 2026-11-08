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

static void CheckCommon(const char *name, const ReaderControls &c) {
  std::string n(name);
  const uint32_t rn = c.page_repeat_next_dpad | c.page_repeat_next_circle;
  const uint32_t rp = c.page_repeat_prev_dpad | c.page_repeat_prev_circle;
  test::ExpectEqU((n + " repeat next subset of page_next").c_str(),
                  rn & ~c.page_next, 0u);
  test::ExpectEqU((n + " repeat prev subset of page_prev").c_str(),
                  rp & ~c.page_prev, 0u);
  test::ExpectEqU((n + " repeat keys never jump bookmarks").c_str(),
                  (rn | rp) & (c.bookmark_next | c.bookmark_prev), 0u);
  test::ExpectTrue((n + " repeat next nonzero").c_str(), rn != 0);
  test::ExpectTrue((n + " repeat prev nonzero").c_str(), rp != 0);
}

int main() {
  KeyMap key;
  ReaderControls p = BuildPortraitControls(key);
  ReaderControls l = BuildLandscapeControls(key);
  CheckCommon("portrait", p);
  CheckCommon("landscape", l);

  test::ExpectEqU("portrait next dpad", p.page_repeat_next_dpad, key.ddown);
  test::ExpectEqU("portrait prev dpad", p.page_repeat_prev_dpad, key.dup);
  test::ExpectEqU("portrait next circle", p.page_repeat_next_circle, key.down);
  test::ExpectEqU("portrait prev circle", p.page_repeat_prev_circle, key.up);

  test::ExpectEqU("landscape next dpad", l.page_repeat_next_dpad, key.dright);
  test::ExpectEqU("landscape prev dpad", l.page_repeat_prev_dpad, key.dleft);
  test::ExpectEqU("landscape next circle", l.page_repeat_next_circle, key.right);
  test::ExpectEqU("landscape prev circle", l.page_repeat_prev_circle, key.left);
  test::ExpectEqU("landscape bookmark next", l.bookmark_next,
                  key.down | key.ddown);
  return 0;
}
