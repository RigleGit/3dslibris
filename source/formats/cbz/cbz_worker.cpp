#include "formats/cbz/cbz_worker.h"

#include "book/book.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/debug_runtime_mode.h"

#include <algorithm>

namespace {

static const size_t kCbzMaxEntryBytes = 64u * 1024u * 1024u;
static const size_t kCbzWorkerStackBytes = 256u * 1024u;
static const int kCbzPreviewBoundsWidth = 240 - 8;
static const int kCbzPreviewBoundsHeight = 320 - 8;
static const int kCbzTopScreenWidth = 240;
static const int kCbzTopScreenHeight = 400;

bool BuildCbzSlotFromPage(const std::string &archive_path,
                          const std::vector<CbzPageEntry> &entries,
                          int page_index, int zoom_index, int max_zoom_index,
                          Book::CbzState::AdjacentSlot *out) {
  if (!out || page_index < 0 || page_index >= (int)entries.size())
    return false;

  std::vector<unsigned char> bytes;
  if (!ReadCbzArchiveEntryBytes(archive_path, entries[(size_t)page_index], &bytes,
                                kCbzMaxEntryBytes)) {
    return false;
  }

  CbzDecodedPage decoded;
  int decode_zoom_index = std::min(zoom_index, max_zoom_index);
  bool decoded_ok = false;
  for (int try_zoom = decode_zoom_index; try_zoom >= 0; --try_zoom) {
    if (DecodeCbzPageImage(bytes, try_zoom, &decoded)) {
      decode_zoom_index = try_zoom;
      decoded_ok = true;
      break;
    }
  }
  if (!decoded_ok)
    return false;

  const pdf_view_utils::PreviewLayout preview_layout =
      pdf_view_utils::ComputePreviewLayout(
          (float)decoded.original_width, (float)decoded.original_height,
          kCbzPreviewBoundsWidth, kCbzPreviewBoundsHeight);
  CbzBitmap preview_bitmap;
  if (!ScaleCbzBitmap(decoded.source_bitmap, std::max(1, preview_layout.width),
                      std::max(1, preview_layout.height), true,
                      &preview_bitmap)) {
    return false;
  }

  const float fit_scale =
      std::min((float)kCbzTopScreenWidth /
                   std::max(1, decoded.original_width),
               (float)kCbzTopScreenHeight /
                   std::max(1, decoded.original_height));
  const float zoom = pdf_view_utils::ZoomForIndex(zoom_index);
  const int interactive_width = std::max(
      1, std::min(decoded.source_bitmap.width,
                  (int)(decoded.original_width * fit_scale * zoom + 0.5f)));
  const int interactive_height = std::max(
      1, std::min(decoded.source_bitmap.height,
                  (int)(decoded.original_height * fit_scale * zoom + 0.5f)));
  CbzBitmap interactive_bitmap;
  // Preloaded interactive tiles are latency-oriented. Nearest-neighbor here
  // avoids paying bilinear scaling cost during background preparation.
  if (!ScaleCbzBitmap(decoded.source_bitmap, interactive_width,
                      interactive_height, false, &interactive_bitmap)) {
    return false;
  }

  out->page = page_index;
  out->zoom_index = zoom_index;
  out->page_width = (float)decoded.original_width;
  out->page_height = (float)decoded.original_height;
  out->preview.page = page_index;
  out->preview.zoom_index = -1;
  out->preview.bitmap_width = preview_bitmap.width;
  out->preview.bitmap_height = preview_bitmap.height;
  out->preview.pixels.swap(preview_bitmap.pixels);
  out->interactive.page = page_index;
  out->interactive.zoom_index = zoom_index;
  out->interactive.bitmap_width = interactive_bitmap.width;
  out->interactive.bitmap_height = interactive_bitmap.height;
  out->interactive.pixels.swap(interactive_bitmap.pixels);
  return true;
}

void CbzWorkerThreadFunc(void *arg) {
  Book::CbzState *cbz_state = static_cast<Book::CbzState *>(arg);
  Book::CbzState::CbzWorker *w = cbz_state->worker;

  while (true) {
    LightEvent_Wait(&w->submit_event);
    LightEvent_Clear(&w->submit_event);

    if (__atomic_load_n(&w->shutdown_requested, __ATOMIC_ACQUIRE))
      break;
    if (!__atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
      continue;

    ResetCbzAdjacentSlot(&w->result_slot);
    w->job_result = BuildCbzSlotFromPage(
        w->job_archive_path, *w->job_entries, w->job_page_index,
        w->job_zoom_index, cbz_state->max_zoom_index, &w->result_slot);

    __atomic_store_n(&w->job_pending, false, __ATOMIC_RELEASE);
    LightEvent_Signal(&w->done_event);
  }
}

int FindCbzPreloadTarget(const Book::CbzState *cbz_state, int current_page) {
  if (!cbz_state || cbz_state->page_count == 0)
    return -1;

  const int next = current_page + 1;
  if (next < (int)cbz_state->page_count && cbz_state->failed_page != next &&
      !(cbz_state->next_slot.page == next &&
        CbzPreviewCacheValid(cbz_state->next_slot.preview, next) &&
        CbzBitmapCacheValid(cbz_state->next_slot.interactive, next,
                            cbz_state->zoom_index))) {
    return next;
  }

  const int prev = current_page - 1;
  if (prev >= 0 && cbz_state->failed_page != prev &&
      !(cbz_state->prev_slot.page == prev &&
        CbzPreviewCacheValid(cbz_state->prev_slot.preview, prev) &&
        CbzBitmapCacheValid(cbz_state->prev_slot.interactive, prev,
                            cbz_state->zoom_index))) {
    return prev;
  }

  return -1;
}

} // namespace

void InitCbzWorker(Book::CbzState *cbz_state) {
  if (debug_runtime::BackgroundWorkersDisabled()) {
    if (cbz_state)
      cbz_state->worker_init_attempted = true;
    return;
  }
  if (!cbz_state || !cbz_state->is_new_3ds)
    return;
  cbz_state->worker_init_attempted = true;

  Book::CbzState::CbzWorker *w = new Book::CbzState::CbzWorker();
  LightEvent_Init(&w->submit_event, RESET_STICKY);
  LightEvent_Init(&w->done_event, RESET_STICKY);

  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  cbz_state->worker = w;
  // MuPDF/stb decode and scaler paths are stack-hungry in debug builds.
  // Give the preload worker real headroom so a failed decode does not scribble
  // process state and surface later as an unrelated HID crash.
  w->thread_handle = threadCreate(CbzWorkerThreadFunc, cbz_state,
                                  kCbzWorkerStackBytes, prio + 1, 1, false);
  if (!w->thread_handle) {
    delete w;
    cbz_state->worker = NULL;
    return;
  }
}

void ShutdownCbzWorker(Book::CbzState *cbz_state) {
  if (!cbz_state || !cbz_state->worker)
    return;
  Book::CbzState::CbzWorker *w = cbz_state->worker;
  __atomic_store_n(&w->shutdown_requested, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&w->submit_event);
  if (w->thread_handle) {
    threadJoin(w->thread_handle, U64_MAX);
    threadFree(w->thread_handle);
    w->thread_handle = NULL;
  }
  delete w;
  cbz_state->worker = NULL;
}

CbzPreloadPumpResult PumpCbzPreloadWorker(Book::CbzState *cbz_state,
                                          int current_page) {
  if (!cbz_state || current_page < 0 || current_page >= (int)cbz_state->page_count)
    return CbzPreloadPumpResult::Idle;

  Book::CbzState::CbzWorker *w = cbz_state->worker;
  CbzPreloadPumpResult result = CbzPreloadPumpResult::Idle;

  if (w && w->job_submitted &&
      !__atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE)) {
    LightEvent_Wait(&w->done_event);
    LightEvent_Clear(&w->done_event);
    w->job_submitted = false;

    if (w->job_result) {
      Book::CbzState::AdjacentSlot *dst = NULL;
      if (w->result_slot.page == current_page + 1)
        dst = &cbz_state->next_slot;
      else if (w->result_slot.page == current_page - 1)
        dst = &cbz_state->prev_slot;

      if (dst) {
        ResetCbzAdjacentSlot(dst);
        *dst = w->result_slot;
        w->result_slot = Book::CbzState::AdjacentSlot();
        result = CbzPreloadPumpResult::Integrated;
      } else {
        ResetCbzAdjacentSlot(&w->result_slot);
      }
    } else {
      ResetCbzAdjacentSlot(&w->result_slot);
    }
  }

  const int target_page = FindCbzPreloadTarget(cbz_state, current_page);
  cbz_state->preload_pending = (target_page >= 0);
  if (target_page < 0)
    return result;

  if (!w)
    return result;
  if (w->job_submitted || __atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
    return result;

  w->job_page_index = target_page;
  w->job_zoom_index = cbz_state->zoom_index;
  w->job_archive_path = cbz_state->archive_path;
  w->job_entries = &cbz_state->entries;
  w->job_result = false;
  ResetCbzAdjacentSlot(&w->result_slot);
  LightEvent_Clear(&w->done_event);
  __atomic_store_n(&w->job_pending, true, __ATOMIC_RELEASE);
  w->job_submitted = true;
  LightEvent_Signal(&w->submit_event);
  return CbzPreloadPumpResult::Submitted;
}
