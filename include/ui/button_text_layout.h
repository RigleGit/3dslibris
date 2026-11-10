#pragma once

// Vertical placement of a multi-line label block inside a button. Pure, so the
// geometry can be checked on the host for every row pitch.
namespace button_text_layout {

// The skin's border (with the compact focus ring) and drop shadow take about
// this much at the top and at the bottom of the button box (the bottom is
// shallower because the focus ring and shadow sit on the lower edge).
static const int kInsetTop = 6;
static const int kInsetBottom = 4;
static const int kInset = kInsetTop + kInsetBottom; // both sides together

struct Block {
  int pitch;          // baseline-to-baseline distance
  int first_baseline; // first baseline, relative to the top of the box
  bool fits;          // false: even the tightest pitch overflows; shrink text
};

inline int InkTop(int ascent) { return (ascent * 3 + 3) / 4; }

// `lines` >= 2. Centers the block inside the box; tightens the pitch from the
// natural line height down to ink_top + descent when the box is short.
inline Block Compute(int box_h, int lines, int line_h, int ascent, int descent) {
  Block b;
  const int avail = box_h - kInset;
  const int ink_top = InkTop(ascent);
  const int min_pitch = ink_top + descent;
  const int need = (lines - 1) * min_pitch + ink_top + descent;
  b.fits = need <= avail;
  int pitch = line_h;
  const int tight = (avail - ink_top - descent) / (lines - 1);
  if (tight < pitch)
    pitch = tight;
  if (pitch < min_pitch)
    pitch = min_pitch;
  const int ink = (lines - 1) * pitch + ink_top + descent;
  b.pitch = pitch;
  b.first_baseline = kInsetTop + (avail - ink) / 2 + ink_top;
  return b;
}

// Box height that holds `lines` lines at the natural line pitch. Callers that
// wrap labels use this to grow the button instead of dropping lines.
inline int HeightForLines(int lines, int line_h, int ascent, int descent,
                          int min_h) {
  if (lines < 1)
    lines = 1;
  int h = (lines - 1) * line_h + InkTop(ascent) + descent + kInset;
  // Single lines are centered with a 2px upward bias, give them room for it.
  if (lines == 1)
    h += 2;
  return h < min_h ? min_h : h;
}

} // namespace button_text_layout
