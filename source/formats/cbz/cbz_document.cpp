#include "formats/cbz/cbz_document.h"

#include "book/book.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"
#include "formats/cbz/cbz_worker.h"
#include "formats/common/book_error.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"

#include <3ds.h>
#include <algorithm>
#include <cstring>

namespace {

static const int kCoverThumbW = 85;
static const int kCoverThumbH = 115;

bool DetectCbzNew3ds() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

bool AssignCoverFromCbzBitmap(Book *book, const CbzBitmap &bitmap) {
  if (!book || bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty())
    return false;

  float scale_x = (float)bitmap.width / (float)kCoverThumbW;
  float scale_y = (float)bitmap.height / (float)kCoverThumbH;
  float scale = std::max(scale_x, scale_y);
  if (scale < 1.0f)
    scale = 1.0f;

  const int dst_w =
      std::max(1, std::min(kCoverThumbW, (int)(bitmap.width / scale)));
  const int dst_h =
      std::max(1, std::min(kCoverThumbH, (int)(bitmap.height / scale)));

  CbzBitmap scaled;
  if (!ScaleCbzBitmap(bitmap, dst_w, dst_h, true, &scaled) ||
      scaled.width <= 0 || scaled.height <= 0 || scaled.pixels.empty()) {
    return false;
  }

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = NULL;
  }
  book->coverPixels =
      new u16[(size_t)scaled.width * (size_t)scaled.height];
  if (!book->coverPixels)
    return false;

  memcpy(book->coverPixels, scaled.pixels.data(),
         scaled.pixels.size() * sizeof(u16));
  book->coverWidth = scaled.width;
  book->coverHeight = scaled.height;
  return true;
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
  cbz_state->viewport.max_zoom_index = policy.max_zoom_index;
  cbz_state->viewport.zoom_index = policy.default_zoom_index;
  cbz_state->viewport.center_x = 0.5f;
  cbz_state->viewport.center_y = 0.5f;
  InitCbzWorker(cbz_state);
}

uint8_t ParseCbzFile(Book *book, const char *path) {
  if (!book || !path)
    return 255;

  std::vector<CbzPageEntry> entries;
  if (!IndexCbzArchiveEntries(path, &entries)) {
    DBG_LOGF_CAT(book->GetStatusReporter(), DBG_LEVEL_WARN, DBG_CAT_RENDER,
                 "CBZ open failed path=%s reason=%s", path,
                 GetLastCbzArchiveError());
    return BOOK_ERR_CORRUPT;
  }

  book->ClearChapters();
  book->ClearTocConfidence();

  std::vector<CbzComicInfoBookmark> bookmarks;
  if (ReadComicInfoBookmarks(path, &bookmarks)) {
    for (const CbzComicInfoBookmark &bm : bookmarks) {
      if (bm.image_index >= 0 &&
          (size_t)bm.image_index < entries.size()) {
        book->AddChapter((u16)bm.image_index, bm.title);
      }
    }
    if (!book->GetChapters().empty()) {
      book->SetTocConfidence(
          TOC_QUALITY_STRONG,
          (u16)std::min<size_t>(book->GetChapters().size(), 65535), 0, 0);
    }
  }

  book->InitCbzView(path, entries, DetectCbzNew3ds());
#ifdef DSLIBRIS_DEBUG
  if (debug_runtime::ForceSynchronousCbzDecode() && book->GetStatusReporter()) {
    DBG_LOG(book->GetStatusReporter(), "CBZ decode path: synchronous");
  }
#endif
  return 0;
}

uint8_t IndexCbzMetadata(Book *book, const char *path) {
  if (!book || !path)
    return 255;
  std::vector<CbzPageEntry> entries;
  if (IndexCbzArchiveEntries(path, &entries))
    return 0;
  DBG_LOGF_CAT(book->GetStatusReporter(), DBG_LEVEL_WARN, DBG_CAT_RENDER,
               "CBZ metadata index failed path=%s reason=%s", path,
               GetLastCbzArchiveError());
  return BOOK_ERR_CORRUPT;
}

int cbz_extract_cover(Book *book, const std::string &cbzpath) {
  if (!book || cbzpath.empty())
    return 1;

  std::vector<CbzPageEntry> entries;
  if (!IndexCbzArchiveEntries(cbzpath, &entries) || entries.empty())
    return 2;

  std::vector<unsigned char> bytes;
  if (!ReadCbzArchiveEntryBytes(cbzpath, entries[0], &bytes,
                                64u * 1024u * 1024u)) {
    return 3;
  }

  CbzDecodedPage decoded;
  if (!DecodeCbzPageImage(bytes, 0, &decoded) ||
      decoded.source_bitmap.width <= 0 || decoded.source_bitmap.height <= 0 ||
      decoded.source_bitmap.pixels.empty()) {
    return 4;
  }

  return AssignCoverFromCbzBitmap(book, decoded.source_bitmap) ? 0 : 5;
}
