#include "reader/fixed_layout_input_utils.h"

namespace reader_input_utils {

bool FixedLayoutSupportsShoulderPageTurn(format_t format) {
  return format == FORMAT_PDF || format == FORMAT_CBZ;
}

uint32_t ReflowableNextPageKeys(uint32_t face_a, uint32_t shoulder_r,
                                uint32_t circle_down, uint32_t dpad_down,
                                uint32_t shoulder_zl) {
  return face_a | shoulder_r | circle_down | dpad_down | shoulder_zl;
}

uint32_t ReflowablePrevPageKeys(uint32_t face_b, uint32_t shoulder_l,
                                uint32_t circle_up, uint32_t dpad_up,
                                uint32_t shoulder_zr) {
  return face_b | shoulder_l | circle_up | dpad_up | shoulder_zr;
}

uint32_t ReflowableBookmarkPrevKeys(uint32_t circle_right,
                                    uint32_t dpad_right) {
  return circle_right | dpad_right;
}

uint32_t ReflowableBookmarkNextKeys(uint32_t circle_left,
                                    uint32_t dpad_left) {
  return circle_left | dpad_left;
}

uint32_t ReflowablePageRepeatKeys(uint32_t circle_dir, uint32_t dpad_dir,
                                  bool include_circle_pad) {
  return include_circle_pad ? (circle_dir | dpad_dir) : dpad_dir;
}

} // namespace reader_input_utils
