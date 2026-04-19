#pragma once

#include "shared/app_flow_utils.h"
#include "shared/status_reporter.h"

#include <vector>

struct Book::MuPdfState {
  struct BitmapCache {
    int page;
    int zoom_index;
    float left;
    float top;
    float width;
    float height;
    int bitmap_width;
    int bitmap_height;
    std::vector<u16> pixels;

    BitmapCache()
        : page(-1), zoom_index(-1), left(0.0f), top(0.0f), width(1.0f),
          height(1.0f), bitmap_width(0), bitmap_height(0) {}
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
    std::vector<u16> partial_pixels;

    IncrementalRenderState()
        : active(false), target_page(-1), target_zoom_index(-1),
          strips_completed(0), strips_total(8), partial_width(0),
          partial_height(0) {}
  };

  struct MuPdfWorker {
    fz_context *worker_ctx;

    int job_page_index;
    float job_scale;
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
          job_display_list(NULL), job_strip_y0(0), job_strip_y1(0),
          job_full_width(0), job_full_height(0), job_pixel_buf(NULL),
          job_result(false), shutdown_requested(false), job_pending(false),
          job_submitted(false), thread_handle(NULL) {}
  };

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
  int max_zoom_index;
  int zoom_index;
  float viewport_center_x;
  float viewport_center_y;
  bool viewport_interaction_active;
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

  MuPdfState()
      : ctx(NULL), doc(NULL), outline(NULL), page_count(0),
        page_width(612.0f), page_height(792.0f), page_width_cache(),
        page_height_cache(), page_metrics_valid(), is_new_3ds(false),
        document_kind(app_flow_utils::MuPdfDocumentKind::Unknown),
        keep_preview_cache(true), keep_tile_cache(false), max_zoom_index(3),
        zoom_index(2), viewport_center_x(0.5f), viewport_center_y(0.5f),
        viewport_interaction_active(false),
        current_preview(), current_interactive_tile(), current_final_zoom(),
        final_cache_pending(false), cached_display_list(NULL),
        cached_display_list_page(-1), prev_slot(), next_slot(), incremental(),
        worker(NULL), worker_init_attempted(false), reporter(NULL) {}
};
