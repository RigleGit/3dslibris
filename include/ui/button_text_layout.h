#pragma once

// Vertical placement of a multi-line label block inside a button. Pure, so the
// geometry can be checked on the host for every row pitch.
namespace button_text_layout {

// Space the skin takes at the top and the bottom of the button box: border,
// focus ring and drop shadow. Compact boxes (<= kCompactMax tall) draw the ring
// against the border (see ui_button_skin.cpp), taller ones keep it inset.
static const int kCompactMax = 36;
inline int InsetTop(int box_h) { return box_h <= kCompactMax ? 6 : 8; }
inline int InsetBottom(int box_h) { return box_h <= kCompactMax ? 4 : 8; }
inline int Insets(int box_h) { return InsetTop(box_h) + InsetBottom(box_h); }

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
  const int avail = box_h - Insets(box_h);
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
  b.first_baseline = InsetTop(box_h) + (avail - ink) / 2 + ink_top;
  return b;
}

// Box height that holds `lines` lines at the natural line pitch. Callers that
// wrap labels use this to grow the button instead of dropping lines.
inline int HeightForLines(int lines, int line_h, int ascent, int descent,
                          int min_h) {
  if (lines < 1)
    lines = 1;
  const int ink = (lines - 1) * line_h + InkTop(ascent) + descent;
  // Single lines are centered with a 2px upward bias, give them room for it.
  const int bias = lines == 1 ? 2 : 0;
  int h = ink + bias + Insets(kCompactMax);
  if (h > kCompactMax)
    h = ink + bias + Insets(kCompactMax + 1);
  return h < min_h ? min_h : h;
}

} // namespace button_text_layout
