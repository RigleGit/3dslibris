#include "formats/cbz/cbz_document.h"

#include "book/book.h"
#include "book/cover_layout_constants.h"
#include "formats/cbz/cbz_archive.h"
#include "formats/cbz/cbz_decode.h"
#include "formats/cbz/cbz_worker.h"
#include "formats/common/book_error.h"
#include "formats/common/format_limits.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "shared/aspect_fit_utils.h"

#include <3ds.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace {

bool DetectCbzNew3ds() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

bool IsValidCbzBitmap(const CbzBitmap &bitmap) {
  return bitmap.width > 0 && bitmap.height > 0 && !bitmap.pixels.empty();
}

void ComputeCoverThumbSize(const CbzBitmap &bitmap, int *dst_w, int *dst_h) {
  const aspect_fit_utils::Placement placement =
      aspect_fit_utils::FitInsideBox(
          0, 0, cover_layout::kBrowserCoverThumbWidth,
          cover_layout::kBrowserCoverThumbHeight, bitmap.width, bitmap.height,
          false);
  *dst_w = placement.width;
  *dst_h = placement.height;
}

bool ReplaceBookCoverPixels(Book *book, const CbzBitmap &bitmap) {
  if (!book || !IsValidCbzBitmap(bitmap))
    return false;

  const size_t pixel_count = bitmap.pixels.size();
  u16 *new_pixels = new (std::nothrow) u16[pixel_count];
  if (!new_pixels)
    return false;

  memcpy(new_pixels, bitmap.pixels.data(), pixel_count * sizeof(u16));

  delete[] book->coverPixels;
  book->coverPixels = new_pixels;
  book->coverWidth = bitmap.width;
  book->coverHeight = bitmap.height;
  return true;
}

bool AssignCoverFromCbzBitmap(Book *book, const CbzBitmap &bitmap) {
  if (!book || !IsValidCbzBitmap(bitmap))
    return false;

  int dst_w = 0;
  int dst_h = 0;
  ComputeCoverThumbSize(bitmap, &dst_w, &dst_h);

  CbzBitmap scaled;
  if (!ScaleCbzBitmap(bitmap, dst_w, dst_h, true, &scaled) ||
      !IsValidCbzBitmap(scaled)) {
    return false;
  }

  return ReplaceBookCoverPixels(book, scaled);
}

bool IndexCbzEntriesWithLog(Book *book, const char *path,
                            std::vector<CbzPageEntry> *entries,
                            const char *context) {
  if (!entries)
    return false;

  if (IndexCbzArchiveEntries(path, entries))
    return true;

  DBG_LOGF_CAT(book ? book->GetStatusReporter() : NULL, DBG_LEVEL_WARN,
               DBG_CAT_RENDER, "CBZ %s failed path=%s reason=%s",
               context ? context : "index", path ? path : "",
               GetLastCbzArchiveError());
  return false;
}

void LoadCbzChaptersFromComicInfo(Book *book, const char *path,
                                  const std::vector<CbzPageEntry> &entries) {
  if (!book || !path || entries.empty())
    return;

  std::vector<CbzComicInfoBookmark> bookmarks;
  if (!ReadComicInfoBookmarks(path, &bookmarks))
    return;

  for (const CbzComicInfoBookmark &bm : bookmarks) {
    if (bm.image_index >= 0 && (size_t)bm.image_index < entries.size())
      book->AddChapter((u16)bm.image_index, bm.title);
  }

  if (!book->GetChapters().empty()) {
    book->SetTocConfidence(
        TOC_QUALITY_STRONG,
        (u16)std::min<size_t>(book->GetChapters().size(), 65535), 0, 0);
  }
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
  if (!IndexCbzEntriesWithLog(book, path, &entries, "open"))
    return BOOK_ERR_CORRUPT;

  book->ClearChapters();
  book->ClearTocConfidence();

  LoadCbzChaptersFromComicInfo(book, path, entries);

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
  if (IndexCbzEntriesWithLog(book, path, &entries, "metadata index"))
    return 0;

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
                                format_limits::kMaxCbzCoverEntryBytes)) {
    return 3;
  }

  CbzDecodedPage decoded;
  if (!DecodeCbzPageImage(bytes, 0, &decoded) ||
      !IsValidCbzBitmap(decoded.source_bitmap)) {
    return 4;
  }

  return AssignCoverFromCbzBitmap(book, decoded.source_bitmap) ? 0 : 5;
}
