#include "reader/page_repeat_utils.h"

namespace reader {

void ResetPageRepeat(PageRepeatState *state) {
  if (!state)
    return;
  state->action = PAGE_REPEAT_NONE;
  state->hold_started_ms = 0;
  state->last_repeat_ms = 0;
}

bool ShouldFirePageRepeat(PageRepeatState *state, PageRepeatAction action,
                          bool down_now, bool held_now, uint64_t now_ms,
                          uint64_t initial_delay_ms,
                          uint64_t repeat_interval_ms) {
  if (!state || action == PAGE_REPEAT_NONE) {
    return false;
  }

  if (!held_now) {
    ResetPageRepeat(state);
    return false;
  }

  if (down_now) {
    state->action = action;
    state->hold_started_ms = now_ms;
    state->last_repeat_ms = now_ms;
    return true;
  }

  if (state->action != action) {
    state->action = action;
    state->hold_started_ms = now_ms;
    state->last_repeat_ms = now_ms;
    return false;
  }

  if (state->hold_started_ms == 0)
    state->hold_started_ms = now_ms;
  if (state->last_repeat_ms == 0)
    state->last_repeat_ms = state->hold_started_ms;

  if (now_ms < state->hold_started_ms + initial_delay_ms)
    return false;
  if (now_ms < state->last_repeat_ms + repeat_interval_ms)
    return false;

  state->last_repeat_ms = now_ms;
  return true;
}

} // namespace reader
