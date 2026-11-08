#pragma once

#include <stdint.h>

namespace reader {

enum PageRepeatAction {
  PAGE_REPEAT_NONE = 0,
  PAGE_REPEAT_NEXT,
  PAGE_REPEAT_PREVIOUS
};

struct PageRepeatState {
  PageRepeatAction action;
  uint64_t hold_started_ms;
  uint64_t last_repeat_ms;

  PageRepeatState()
      : action(PAGE_REPEAT_NONE), hold_started_ms(0), last_repeat_ms(0) {}
};

void ResetPageRepeat(PageRepeatState *state);

bool ShouldFirePageRepeat(PageRepeatState *state, PageRepeatAction action,
                          bool down_now, bool held_now, uint64_t now_ms,
                          uint64_t initial_delay_ms,
                          uint64_t repeat_interval_ms);

// Resolves which repeat action (if any) fires this frame for a pair of
// direction masks. Any held or newly pressed key outside next|prev cancels
// repeat. Returns PAGE_REPEAT_NONE when nothing fires.
PageRepeatAction ResolveDirectionalRepeat(PageRepeatState *state,
                                          uint32_t keys_down,
                                          uint32_t keys_held,
                                          uint32_t next_keys,
                                          uint32_t prev_keys, uint64_t now_ms,
                                          uint64_t initial_delay_ms,
                                          uint64_t repeat_interval_ms);

} // namespace reader
