// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace pdf_view_utils {

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
int DefaultZoomIndex();
float ZoomForIndex(int zoom_index);

PreviewLayout ComputePreviewLayout(float page_width, float page_height,
                                   int preview_width, int preview_height);
NormalizedRect ComputeViewportRect(float page_width, float page_height,
                                   float zoom, float screen_width,
                                   float screen_height, float center_x,
                                   float center_y);
NormalizedPoint RecenterViewportFromPreview(const PreviewLayout &preview,
                                            const NormalizedRect &viewport,
                                            int touch_x, int touch_y);

} // namespace pdf_view_utils
