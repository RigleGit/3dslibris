#pragma once

#include <algorithm>
#include <cmath>

namespace fixed_layout_viewport_utils {

enum PageTurnDirection {
  PAGE_TURN_LEFT_TO_RIGHT = 0,
  PAGE_TURN_RIGHT_TO_LEFT = 1,
};

struct ViewportCenter {
  float x;
  float y;
};

inline ViewportCenter DefaultPageTurnViewportCenter(
    PageTurnDirection direction = PAGE_TURN_LEFT_TO_RIGHT) {
  ViewportCenter center = {0.0f, 0.0f};
  if (direction == PAGE_TURN_RIGHT_TO_LEFT)
    center.x = 1.0f;
  return center;
}

struct ViewportState {
  int zoom_index;
  int max_zoom_index;
  float center_x;
  float center_y;
  bool interaction_active;

  ViewportState()
      : zoom_index(2), max_zoom_index(3),
        center_x(0.5f), center_y(0.5f),
        interaction_active(false) {}
};

inline float ClampCenter(float v) {
  return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

inline int ClampZoomIndex(int zoom_index, int max_zoom) {
  return std::max(0, std::min(zoom_index, max_zoom));
}

inline bool PanViewport(ViewportState *state, float dx, float dy) {
  if (!state) return false;
  const float new_x = ClampCenter(state->center_x + dx);
  const float new_y = ClampCenter(state->center_y + dy);
  if (std::abs(new_x - state->center_x) < 0.0005f &&
      std::abs(new_y - state->center_y) < 0.0005f)
    return false;
  state->center_x = new_x;
  state->center_y = new_y;
  return true;
}

inline bool AdjustZoom(ViewportState *state, int step) {
  if (!state || step == 0) return false;
  const int new_zoom = ClampZoomIndex(state->zoom_index + step, state->max_zoom_index);
  if (new_zoom == state->zoom_index) return false;
  state->zoom_index = new_zoom;
  return true;
}

inline void ResetViewport(ViewportState *state,
                          PageTurnDirection direction = PAGE_TURN_LEFT_TO_RIGHT) {
  if (!state) return;
  const ViewportCenter center = DefaultPageTurnViewportCenter(direction);
  state->center_x = center.x;
  state->center_y = center.y;
  state->interaction_active = false;
}

inline void ResetViewportForTargetChange(ViewportState *state,
                                         int fit_zoom_index) {
  if (!state)
    return;
  state->zoom_index = ClampZoomIndex(fit_zoom_index, state->max_zoom_index);
  state->center_x = 0.5f;
  state->center_y = 0.5f;
  state->interaction_active = false;
}

} // namespace fixed_layout_viewport_utils
