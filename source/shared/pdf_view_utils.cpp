// SPDX-License-Identifier: AGPL-3.0-or-later

#include "shared/pdf_view_utils.h"

#include <algorithm>
#include <cstdlib>

namespace pdf_view_utils {
namespace {

static const float kZoomPresets[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f};

float Clamp01(float value) {
  if (value < 0.0f)
    return 0.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

float ClampCenter(float center, float span) {
  const float half = span * 0.5f;
  if (span >= 1.0f)
    return 0.5f;
  if (center < half)
    return half;
  if (center > 1.0f - half)
    return 1.0f - half;
  return center;
}

} // namespace

int ClampZoomIndex(int zoom_index) {
  if (zoom_index < 0)
    return 0;
  const int max_index = (int)(sizeof(kZoomPresets) / sizeof(kZoomPresets[0])) - 1;
  if (zoom_index > max_index)
    return max_index;
  return zoom_index;
}

DevicePolicy GetDevicePolicy(bool is_new_3ds) {
  DevicePolicy policy;
  policy.default_zoom_index = 2;
  policy.max_zoom_index = is_new_3ds ? 5 : 3;
  policy.keep_preview_cache = true;
  policy.keep_tile_cache = is_new_3ds;
  policy.mupdf_store_bytes =
      is_new_3ds ? (20u * 1024u * 1024u) : (4u * 1024u * 1024u);
  return policy;
}

int ClampZoomIndexForDevice(int zoom_index, bool is_new_3ds) {
  const DevicePolicy policy = GetDevicePolicy(is_new_3ds);
  const int clamped = ClampZoomIndex(zoom_index);
  if (clamped > policy.max_zoom_index)
    return policy.max_zoom_index;
  return clamped;
}

int DefaultZoomIndex() { return 2; }

float ZoomForIndex(int zoom_index) {
  return kZoomPresets[ClampZoomIndex(zoom_index)];
}

PreviewLayout ComputePreviewLayout(float page_width, float page_height,
                                   int preview_width, int preview_height) {
  PreviewLayout out = {0, 0, 0, 0};
  if (page_width <= 0.0f || page_height <= 0.0f || preview_width <= 0 ||
      preview_height <= 0) {
    return out;
  }

  const float sx = (float)preview_width / page_width;
  const float sy = (float)preview_height / page_height;
  const float scale = std::min(sx, sy);
  out.width = (int)(page_width * scale + 0.5f);
  out.height = (int)(page_height * scale + 0.5f);
  out.x = (preview_width - out.width) / 2;
  out.y = (preview_height - out.height) / 2;
  return out;
}

PreviewLayout ComputePreviewLayoutInBounds(float page_width, float page_height,
                                           int bounds_x, int bounds_y,
                                           int bounds_width,
                                           int bounds_height) {
  PreviewLayout out =
      ComputePreviewLayout(page_width, page_height, bounds_width, bounds_height);
  out.x += bounds_x;
  out.y += bounds_y;
  return out;
}

NormalizedRect ComputeViewportRect(float page_width, float page_height,
                                   float zoom, float screen_width,
                                   float screen_height, float center_x,
                                   float center_y) {
  NormalizedRect out = {0.0f, 0.0f, 1.0f, 1.0f};
  if (page_width <= 0.0f || page_height <= 0.0f || screen_width <= 0.0f ||
      screen_height <= 0.0f || zoom <= 0.0f) {
    return out;
  }

  const float fit_scale =
      std::min(screen_width / page_width, screen_height / page_height);
  if (fit_scale <= 0.0f)
    return out;

  const float scaled_page_width = page_width * fit_scale * zoom;
  const float scaled_page_height = page_height * fit_scale * zoom;
  const float visible_page_width =
      std::min(page_width, screen_width / (fit_scale * zoom));
  const float visible_page_height =
      std::min(page_height, screen_height / (fit_scale * zoom));

  (void)scaled_page_width;
  (void)scaled_page_height;

  out.width = Clamp01(visible_page_width / page_width);
  out.height = Clamp01(visible_page_height / page_height);

  const float clamped_center_x = ClampCenter(center_x, out.width);
  const float clamped_center_y = ClampCenter(center_y, out.height);
  out.left = clamped_center_x - out.width * 0.5f;
  out.top = clamped_center_y - out.height * 0.5f;
  return out;
}

NormalizedPoint RecenterViewportFromPreview(const PreviewLayout &preview,
                                            const NormalizedRect &viewport,
                                            int touch_x, int touch_y) {
  NormalizedPoint out = {0.5f, 0.5f};
  if (preview.width <= 0 || preview.height <= 0)
    return out;

  const float px =
      Clamp01((float)(touch_x - preview.x) / (float)preview.width);
  const float py =
      Clamp01((float)(touch_y - preview.y) / (float)preview.height);
  out.x = ClampCenter(px, viewport.width);
  out.y = ClampCenter(py, viewport.height);
  return out;
}

bool TouchMovementExceedsThreshold(int prev_x, int prev_y, int next_x,
                                   int next_y, int min_delta) {
  const int dx = std::abs(next_x - prev_x);
  const int dy = std::abs(next_y - prev_y);
  return std::max(dx, dy) >= min_delta;
}

} // namespace pdf_view_utils
