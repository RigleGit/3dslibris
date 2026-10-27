#pragma once
#include "formats/common/raster_image_decode.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "ui/button.h"
#include "formats/common/pdf_view_utils.h"
#include <string>
class Book;

// Ephemeral, main-thread-only state. Contains no pointers into page tokens.
struct ReaderImageViewerSession {
  Book *book = nullptr;
  unsigned session = 0, revision = 0, pages = 0;
  unsigned page = 0, token = 0;
  uint16_t image_id = 0;
  int selection = -1;
  fixed_layout_viewport_utils::ViewportState viewport;
  raster_image::Bitmap image, preview;
  image_memory::Bytes source;
  pdf_view_utils::PreviewLayout overview{};
  std::string message;
  bool dirty = true, loading = true, wait_release = true, suspended = false;
  bool preview_ready = false;
  bool panel_dirty = true, panel_enabled = false;
  int panel_orientation = -1, panel_zoom = -1;
  uint16_t panel_fg = 0, panel_bg = 0;
  std::string panel_status;
  enum Control { ZoomIn, ZoomOut, Fit, Back, Library, Settings, ControlCount };
  Button controls[ControlCount];
  bool touch_panning = false;
};
