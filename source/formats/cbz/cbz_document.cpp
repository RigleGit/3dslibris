#include "formats/cbz/cbz_document.h"

#include "book/book.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_worker.h"
#include "formats/common/book_error.h"
#include "shared/pdf_view_utils.h"

#include <3ds.h>
#include <algorithm>

namespace {

bool DetectCbzNew3ds() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

} // namespace

void Book::ResetCbzState() {
  if (!cbz_state)
    return;
  ShutdownCbzWorker(cbz_state);
  delete cbz_state;
  cbz_state = NULL;
}

void Book::InitCbzView(const std::string &archive_path,
                       const std::vector<CbzPageEntry> &entries,
                       bool is_new_3ds) {
  ResetCbzState();
  cbz_state = new CbzState();
  const pdf_view_utils::DevicePolicy policy =
      pdf_view_utils::GetDevicePolicy(is_new_3ds);
  cbz_state->archive_path = archive_path;
  cbz_state->entries = entries;
  cbz_state->page_count = (u16)std::min<size_t>(entries.size(), 65535);
  cbz_state->is_new_3ds = is_new_3ds;
  cbz_state->max_zoom_index = policy.max_zoom_index;
  cbz_state->zoom_index = policy.default_zoom_index;
  cbz_state->viewport_center_x = 0.5f;
  cbz_state->viewport_center_y = 0.5f;
  InitCbzWorker(cbz_state);
}

uint8_t ParseCbzFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  std::vector<CbzPageEntry> entries;
  if (!IndexCbzArchiveEntries(path, &entries))
    return BOOK_ERR_CORRUPT;

  book->ClearChapters();
  book->ClearTocConfidence();
  book->InitCbzView(path, entries, DetectCbzNew3ds());
  return 0;
}

uint8_t IndexCbzMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 255;
  std::vector<CbzPageEntry> entries;
  return IndexCbzArchiveEntries(path, &entries) ? 0 : BOOK_ERR_CORRUPT;
}
