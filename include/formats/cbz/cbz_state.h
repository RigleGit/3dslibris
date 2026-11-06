#pragma once

#include "book/book.h"
#include "formats/common/fixed_layout_bitmap_cache.h"
#include "formats/common/fixed_layout_gpu_renderer.h"
#include "formats/common/fixed_layout_preview_panel.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "formats/cbz/cbz_types.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"

#include <3ds.h>

#include <string>
#include <vector>

struct Book::CbzState {
  struct BitmapCache : fixed_layout_bitmap_cache::Base {
    BitmapCache() : Base() {}
  };

  struct PageBitmap {
    int page;
    int zoom_index;
    int requested_zoom_index;
    int original_width;
    int original_height;
    CbzBitmap bitmap;
    uint64_t generation;
    fixed_layout_gpu::TextureCache gpu;

    PageBitmap()
        : page(-1), zoom_index(-1), requested_zoom_index(-1), original_width(0),
          original_height(0), bitmap(), generation(0), gpu() {}
  };

  struct AdjacentSlot {
    int page;
    int zoom_index;
    float page_width;
    float page_height;
    BitmapCache preview;
    BitmapCache interactive;

    AdjacentSlot()
        : page(-1), zoom_index(-1), page_width(0.0f), page_height(0.0f),
          preview(), interactive() {}
  };

  struct CbzWorker {
    volatile bool shutdown_requested;
    volatile bool job_pending;
    bool job_submitted;
    int job_page_index;
    int job_zoom_index;
    std::string job_archive_path;
    std::vector<CbzPageEntry> *job_entries;
    bool job_result;
    AdjacentSlot result_slot;
    LightEvent submit_event;
    LightEvent done_event;
    Thread thread_handle;

    CbzWorker()
        : shutdown_requested(false), job_pending(false), job_submitted(false),
          job_page_index(-1), job_zoom_index(-1), job_archive_path(),
          job_entries(NULL), job_result(false), result_slot(),
          thread_handle(NULL) {}
  };

  // Main-thread reader; background/cover reads keep their own handles.
  CbzArchiveReader archive_reader;
  fz_context *decode_context;
  std::string archive_path;
  int target_top_width;
  int target_top_height;
  int target_bottom_width;
  int target_bottom_height;
  std::vector<CbzPageEntry> entries;
  u16 page_count;
  bool is_new_3ds;
  fixed_layout_viewport_utils::ViewportState viewport;
  float page_width;
  float page_height;
  PageBitmap current_source;
  BitmapCache current_preview;
  fixed_layout_preview_panel::State preview_panel;
  BitmapCache current_interactive;
  AdjacentSlot prev_slot;
  AdjacentSlot next_slot;
  CbzWorker *worker;
  bool worker_init_attempted;
  bool preload_pending;
  int failed_page;
  int logged_failed_page;
  std::string last_error;
  bool gpu_draw_failed = false;
  bool wide_top_screen = false;

  CbzState()
      : decode_context(NULL), archive_path(), target_top_width(240), target_top_height(400),
        target_bottom_width(240), target_bottom_height(320), entries(),
        page_count(0), is_new_3ds(false),
        viewport(), page_width(1.0f), page_height(1.0f),
        current_source(),
        current_preview(), preview_panel(), current_interactive(), prev_slot(), next_slot(),
        worker(NULL), worker_init_attempted(false), preload_pending(false),
        failed_page(-1), logged_failed_page(-1), last_error() {}
};

inline void ResetCbzBitmapCache(Book::CbzState::BitmapCache *cache) {
  if (!cache)
    return;
  cache->page = -1;
  cache->zoom_index = -1;
  cache->bitmap_width = 0;
  cache->bitmap_height = 0;
  std::vector<u16>().swap(cache->pixels);
}

inline bool CbzBitmapCacheValid(const Book::CbzState::BitmapCache &cache,
                                int page, int zoom_index) {
  return cache.page == page && cache.zoom_index == zoom_index &&
         cache.bitmap_width > 0 && cache.bitmap_height > 0 &&
         !cache.pixels.empty();
}

inline bool CbzPreviewCacheValid(const Book::CbzState::BitmapCache &cache,
                                 int page) {
  return cache.page == page && cache.bitmap_width > 0 &&
         cache.bitmap_height > 0 && !cache.pixels.empty();
}

inline void ResetCbzAdjacentSlot(Book::CbzState::AdjacentSlot *slot) {
  if (!slot)
    return;
  slot->page = -1;
  slot->zoom_index = -1;
  slot->page_width = 0.0f;
  slot->page_height = 0.0f;
  ResetCbzBitmapCache(&slot->preview);
  ResetCbzBitmapCache(&slot->interactive);
}
