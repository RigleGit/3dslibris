#pragma once

#include <cstdint>

// Tap / hold decision for touches on library cells. A short tap opens the
// entry; a touch held past the threshold only selects it (preview), and a
// drag off the cell cancels. The caller feeds one sample per frame.
namespace browser_touch_gesture {

static const uint64_t kHoldThresholdMs = 400;

enum Action {
  ACTION_NONE,
  ACTION_SELECT,  // touch-down on a cell: highlight immediately
  ACTION_PREVIEW, // held past the threshold on the same cell: keep selection
  ACTION_OPEN     // released as a short tap on the same cell
};

struct State {
  bool active;
  bool cancelled;
  bool previewed;
  int cell;
  uint64_t down_ms;
  State() : active(false), cancelled(false), previewed(false), cell(-1), down_ms(0) {}
  void Reset() { *this = State(); }
};

// guarded: input-release guard is armed (touch may be held from a previous
// screen); no gesture can start or finish while it is.
// cell_under: cell index under the finger this frame, -1 for none.
inline Action Step(State &s, bool guarded, bool touching, bool down_edge,
                   int cell_under, uint64_t now_ms) {
  if (guarded) {
    s.Reset();
    return ACTION_NONE;
  }
  if (s.active && down_edge)
    s.Reset(); // a release frame was missed; start over without opening
  if (!s.active) {
    if (!down_edge || !touching || cell_under < 0)
      return ACTION_NONE;
    s.active = true;
    s.cell = cell_under;
    s.down_ms = now_ms;
    return ACTION_SELECT;
  }
  const bool held_long = now_ms >= s.down_ms + kHoldThresholdMs;
  if (touching) {
    if (cell_under != s.cell)
      s.cancelled = true;
    if (!s.cancelled && !s.previewed && held_long) {
      s.previewed = true;
      return ACTION_PREVIEW;
    }
    return ACTION_NONE;
  }
  const Action result =
      (!s.cancelled && !s.previewed && !held_long) ? ACTION_OPEN : ACTION_NONE;
  s.Reset();
  return result;
}

} // namespace browser_touch_gesture
