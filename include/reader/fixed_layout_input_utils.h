#pragma once

#include <stdint.h>

#include "shared/app_flow_utils.h"

namespace reader_input_utils {

bool FixedLayoutSupportsShoulderPageTurn(format_t format);

uint32_t ReflowableNextPageKeys(uint32_t face_a, uint32_t shoulder_r,
                                uint32_t circle_down, uint32_t dpad_down,
                                uint32_t shoulder_zl);
uint32_t ReflowablePrevPageKeys(uint32_t face_b, uint32_t shoulder_l,
                                uint32_t circle_up, uint32_t dpad_up,
                                uint32_t shoulder_zr);
uint32_t ReflowableBookmarkPrevKeys(uint32_t circle_right,
                                    uint32_t dpad_right);
uint32_t ReflowableBookmarkNextKeys(uint32_t circle_left,
                                    uint32_t dpad_left);
uint32_t ReflowablePageRepeatKeys(uint32_t circle_dir, uint32_t dpad_dir,
                                  bool include_circle_pad);

} // namespace reader_input_utils
