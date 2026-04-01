// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace pdf_view_utils {

struct DevicePolicy {
  int default_zoom_index;
  int max_zoom_index;
  bool keep_preview_cache;
  bool keep_tile_cache;
  unsigned int mupdf_store_bytes;
};

struct PreviewLayout {
  int x;
  int y;
  int width;
  int height;
};

struct NormalizedRect {
  float left;
  float top;
  float width;
  float height;
};

struct NormalizedPoint {
  float x;
  float y;
};

int ClampZoomIndex(int zoom_index);
int ClampZoomIndexForDevice(int zoom_index, bool is_new_3ds);
int DefaultZoomIndex();
DevicePolicy GetDevicePolicy(bool is_new_3ds);
float ZoomForIndex(int zoom_index);

PreviewLayout ComputePreviewLayout(float page_width, float page_height,
                                   int preview_width, int preview_height);
PreviewLayout ComputePreviewLayoutInBounds(float page_width, float page_height,
                                           int bounds_x, int bounds_y,
                                           int bounds_width,
                                           int bounds_height);
NormalizedRect ComputeViewportRect(float page_width, float page_height,
                                   float zoom, float screen_width,
                                   float screen_height, float center_x,
                                   float center_y);
NormalizedPoint RecenterViewportFromPreview(const PreviewLayout &preview,
                                            const NormalizedRect &viewport,
                                            int touch_x, int touch_y);
bool TouchMovementExceedsThreshold(int prev_x, int prev_y, int next_x,
                                   int next_y, int min_delta);

} // namespace pdf_view_utils
