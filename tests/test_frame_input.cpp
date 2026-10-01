#include "app/frame_input.h"
#include "shared/orientation_utils.h"

#include <cstdio>
#include <cstdlib>

static void Expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

int main() {
  FrameInput empty;
  Expect(empty.keys_down == 0, "default keys_down");
  Expect(empty.keys_held == 0, "default keys_held");
  Expect(!empty.touch_active, "default touch inactive");
  Expect(empty.touch_raw_x == 0 && empty.touch_raw_y == 0,
         "default touch coordinates");
  Expect(empty.timestamp_ms == 0, "default timestamp");

  // A frame retains distinct pressed/held masks and a full-width clock value.
  const FrameInput input(0x80000004u, 0x4000000cu, true, 100, 50,
                         UINT64_C(0x100000009));
  Expect(input.keys_down == 0x80000004u, "captured keys_down");
  Expect(input.keys_held == 0x4000000cu, "captured keys_held");
  Expect(input.touch_active, "captured touch active");
  Expect(input.timestamp_ms == UINT64_C(0x100000009), "64-bit timestamp");

  const touch_map_utils::TouchPoint mapped =
      input.MapTouch(orientation_utils::ORIENT_TURNED_LEFT);
  Expect(mapped.x == 50 && mapped.y == 219, "mapped captured touch");
  const touch_map_utils::TouchPoint remapped =
      input.MapTouch(orientation_utils::ORIENT_TURNED_RIGHT);
  Expect(remapped.x == 189 && remapped.y == 100,
         "same snapshot uses the requested orientation");
  Expect(input.touch_raw_x == 100 && input.touch_raw_y == 50,
         "mapping preserves raw snapshot for other frame consumers");
  std::printf("All frame_input tests passed.\n");
  return 0;
}
