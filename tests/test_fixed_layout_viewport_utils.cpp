#include "formats/common/fixed_layout_viewport_utils.h"

#include <cstdio>
#include <cstdlib>

int main() {
  fixed_layout_viewport_utils::ViewportCenter center =
      fixed_layout_viewport_utils::DefaultPageTurnViewportCenter();
  if (center.x != 0.0f || center.y != 0.0f) {
    fprintf(stderr, "expected default viewport center to be 0,0\n");
    return 1;
  }
  return 0;
}
