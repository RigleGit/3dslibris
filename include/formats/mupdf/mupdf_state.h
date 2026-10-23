#pragma once

#include "book/book.h"
#include "formats/common/fixed_layout_bitmap_cache.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "shared/app_flow_utils.h"
#include "shared/status_reporter.h"

#include <vector>

struct Book::MuPdfState {
  struct BitmapCache : fixed_layout_bitmap_cache::Base {
    float left;
    float top;
    float width;
    float height;

    BitmapCache()
        : Base(), left(0.0f), top(0.0f), width(1.0f),
          height(1.0f) {}
  };

  struct AdjacentSlot {
    int page;
    fz_display_list *display_list;
    BitmapCache preview;
    BitmapCache interactive_tile;

    AdjacentSlot()
        : page(-1), display_list(NULL), preview(), interactive_tile() {}
  };

  struct IncrementalRenderState {
    bool active;
    int target_page;
    int target_zoom_index;
    int strips_completed;
    int strips_total;
    int partial_width;
    int partial_height;
    // Cached page bounds (PDF units) and scale used for all strips.
    // Avoids redundant fz_load_page on each strip call.
    float bounds_x0, bounds_y0, bounds_x1, bounds_y1;
    float render_scale;
    std::vector<u16> partial_pixels;

    IncrementalRenderState()
        : active(false), target_page(-1), target_zoom_index(-1),
          strips_completed(0), strips_total(8), partial_width(0),
          partial_height(0), bounds_x0(0.0f), bounds_y0(0.0f),
          bounds_x1(0.0f), bounds_y1(0.0f), render_scale(1.0f) {}
  };

  struct MuPdfWorker {
    fz_context *worker_ctx;

    int job_page_index;
    float job_scale;
    float job_bounds_x0, job_bounds_y0, job_bounds_x1, job_bounds_y1;
    fz_display_list *job_display_list;
    int job_strip_y0;
    int job_strip_y1;
    int job_full_width;
    int job_full_height;
    std::vector<u16> *job_pixel_buf;

    bool job_result;

    LightEvent submit_event;
    LightEvent done_event;

    volatile bool shutdown_requested;
    volatile bool job_pending;
    bool job_submitted;

    Thread thread_handle;

    MuPdfWorker()
        : worker_ctx(NULL), job_page_index(-1), job_scale(1.0f),
          job_bounds_x0(0.0f), job_bounds_y0(0.0f),
          job_bounds_x1(0.0f), job_bounds_y1(0.0f),
          job_display_list(NULL), job_strip_y0(0), job_strip_y1(0),
          job_full_width(0), job_full_height(0), job_pixel_buf(NULL),
          job_result(false), shutdown_requested(false), job_pending(false),
          job_submitted(false), thread_handle(NULL) {}
  };

  int target_top_width;
  int target_top_height;
  int target_bottom_width;
  int target_bottom_height;
  fz_context *ctx;
  fz_document *doc;
  fz_outline *outline;
  u16 page_count;
  float page_width;
  float page_height;
  std::vector<float> page_width_cache;
  std::vector<float> page_height_cache;
  std::vector<u8> page_metrics_valid;
  bool is_new_3ds;
  app_flow_utils::MuPdfDocumentKind document_kind;
  bool keep_preview_cache;
  bool keep_tile_cache;
  fixed_layout_viewport_utils::ViewportState viewport;
  BitmapCache current_preview;
  BitmapCache current_interactive_tile;
  BitmapCache current_final_zoom;
  bool final_cache_pending;
  fz_display_list *cached_display_list;
  int cached_display_list_page;
  AdjacentSlot prev_slot;
  AdjacentSlot next_slot;
  IncrementalRenderState incremental;
  MuPdfWorker *worker;
  bool worker_init_attempted;
  IStatusReporter *reporter;
  int page_too_complex_for_device; // page index skipped due to complexity, -1 if none
  int interactive_render_failed_page;
  int interactive_render_failed_zoom;
  int final_render_failed_page;
  int final_render_failed_zoom;

  MuPdfState()
      : target_top_width(240), target_top_height(400),
        target_bottom_width(240), target_bottom_height(320), ctx(NULL),
        doc(NULL), outline(NULL), page_count(0),
        page_width(612.0f), page_height(792.0f), page_width_cache(),
        page_height_cache(), page_metrics_valid(), is_new_3ds(false),
        document_kind(app_flow_utils::MuPdfDocumentKind::Unknown),
        keep_preview_cache(true), keep_tile_cache(false),
        viewport(),
        current_preview(), current_interactive_tile(), current_final_zoom(),
        final_cache_pending(false), cached_display_list(NULL),
        cached_display_list_page(-1), prev_slot(), next_slot(), incremental(),
        worker(NULL), worker_init_attempted(false), reporter(NULL),
        page_too_complex_for_device(-1), interactive_render_failed_page(-1),
        interactive_render_failed_zoom(-1), final_render_failed_page(-1),
        final_render_failed_zoom(-1) {}
};
