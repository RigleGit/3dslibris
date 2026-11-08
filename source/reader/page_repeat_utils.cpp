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

PageRepeatAction ResolveDirectionalRepeat(PageRepeatState *state,
                                          uint32_t keys_down,
                                          uint32_t keys_held,
                                          uint32_t next_keys,
                                          uint32_t prev_keys, uint64_t now_ms,
                                          uint64_t initial_delay_ms,
                                          uint64_t repeat_interval_ms) {
  const uint32_t repeat_keys = next_keys | prev_keys;
  const uint32_t other_held = keys_held & ~repeat_keys;
  const uint32_t other_down = keys_down & ~repeat_keys;
  if (other_held == 0 && other_down == 0 && (keys_held & next_keys)) {
    return ShouldFirePageRepeat(state, PAGE_REPEAT_NEXT,
                                (keys_down & next_keys) != 0, true, now_ms,
                                initial_delay_ms, repeat_interval_ms)
               ? PAGE_REPEAT_NEXT
               : PAGE_REPEAT_NONE;
  }
  if (other_held == 0 && other_down == 0 && (keys_held & prev_keys)) {
    return ShouldFirePageRepeat(state, PAGE_REPEAT_PREVIOUS,
                                (keys_down & prev_keys) != 0, true, now_ms,
                                initial_delay_ms, repeat_interval_ms)
               ? PAGE_REPEAT_PREVIOUS
               : PAGE_REPEAT_NONE;
  }
  if ((keys_held & repeat_keys) == 0 || other_held != 0)
    ResetPageRepeat(state);
  return PAGE_REPEAT_NONE;
}

} // namespace reader
