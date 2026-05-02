#pragma once

#include "book/book.h"
#include "formats/common/fixed_layout_viewport_utils.h"
#include "formats/cbz/cbz_types.h"

#include <3ds.h>

#include <string>
#include <vector>

struct Book::CbzState {
  struct BitmapCache {
    int page;
    int zoom_index;
    int bitmap_width;
    int bitmap_height;
    std::vector<u16> pixels;

    BitmapCache()
        : page(-1), zoom_index(-1), bitmap_width(0), bitmap_height(0),
          pixels() {}
  };

  struct PageBitmap {
    int page;
    int zoom_index;
    int original_width;
    int original_height;
    CbzBitmap bitmap;

    PageBitmap()
        : page(-1), zoom_index(-1), original_width(0), original_height(0),
          bitmap() {}
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

  std::string archive_path;
  std::vector<CbzPageEntry> entries;
  u16 page_count;
  bool is_new_3ds;
  fixed_layout_viewport_utils::ViewportState viewport;
  float page_width;
  float page_height;
  PageBitmap current_source;
  BitmapCache current_preview;
  BitmapCache current_interactive;
  AdjacentSlot prev_slot;
  AdjacentSlot next_slot;
  CbzWorker *worker;
  bool worker_init_attempted;
  bool preload_pending;
  int failed_page;
  int logged_failed_page;
  std::string last_error;

  CbzState()
      : archive_path(), entries(), page_count(0), is_new_3ds(false),
        viewport(), page_width(1.0f), page_height(1.0f),
        current_source(),
        current_preview(), current_interactive(), prev_slot(), next_slot(),
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
  cache->pixels.clear();
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
