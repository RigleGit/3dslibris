#pragma once

namespace fixed_layout_viewport_utils {

struct ViewportCenter {
  float x;
  float y;
};

inline ViewportCenter DefaultPageTurnViewportCenter() {
  ViewportCenter center = {0.0f, 0.0f};
  return center;
}

} // namespace fixed_layout_viewport_utils
