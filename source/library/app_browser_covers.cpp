/*
    3dslibris - app_browser_covers.cpp
    Extracted from app_browser.cpp. Handles in-RAM cover bitmap loading
    and eviction for the visible portion of the library browser grid.
    Disk-side cover persistence lives in source/library/cover_cache.cpp.

    No behavior change — pure code motion. A few small static helpers
    are duplicated from app_browser.cpp's anonymous namespace; they are
    tiny wrappers around browser_view_utils and live there too for use
    by the rest of the browser code. Consolidating into a shared header
    is a follow-up.
*/

#include "app/app.h"
#include "app/library_controller.h"

#include <string>

#include "book/book.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_view_utils.h"
#include "library/cover_cache.h"
#include "settings/prefs.h"
#include "shared/app_flow_utils.h"

namespace {

static const uint8_t kCoverMaxAttempts = cover_cache::kMaxAttempts;

static BrowserViewMode CurrentBrowserViewMode(const App &app) {
  if (!app.prefs)
    return BROWSER_VIEW_GALLERY;
  return app.prefs->browser_view_mode;
}

static int CurrentBrowserPageSize(const App &app) {
  return browser_view_utils::PageSize(CurrentBrowserViewMode(app));
}

static bool ShouldCurrentBrowserLoadCovers(const App &app) {
  return browser_view_utils::ShouldLoadCovers(CurrentBrowserViewMode(app));
}

static std::string BuildBookPath(Book *book) {
  if (!book)
    return "";
  if (book->IsBrowserFolder())
    return book->GetBrowserFolderCoverPath();
  if (!book->GetFolderName() || !book->GetFileName())
    return "";
  std::string path = book->GetFolderName();
  path.push_back('/');
  path.append(book->GetFileName());
  return path;
}

} // namespace

void LibraryController::UnloadNonVisibleBrowserCoverCaches() {
  if (app_.BookCount() <= 0)
    return;

  if (!ShouldCurrentBrowserLoadCovers(app_)) {
    // Returning to gallery starts a fresh cover snapshot, allowing external
    // sidecar edits to be picked up without probing on every page revisit.
    cover_cache::ClearPathCache();
    for (int i = 0; i < app_.BookCount(); i++) {
      Book *book = app_.books[i];
      if (!book)
        continue;
      if (book->IsBrowserFolder() || book->coverPixels) {
        book->coverAttempts = 0;
        book->coverRetryAfterMs = 0;
      }
      if (!book->coverPixels)
        continue;
      delete[] book->coverPixels;
      book->coverPixels = nullptr;
      book->coverWidth = 0;
      book->coverHeight = 0;
    }
    return;
  }

  const browser_cover_cache_utils::VisibleRange visible =
      browser_cover_cache_utils::ComputeVisibleRange(
          app_.GetBrowserPageStart(), app_.BookCount(),
          CurrentBrowserPageSize(app_));
  for (int i = 0; i < app_.BookCount(); i++) {
    if (browser_cover_cache_utils::RangeContains(visible, i))
      continue;

    Book *book = app_.books[i];
    if (!book)
      continue;
    if (book->IsBrowserFolder() || book->coverPixels) {
      book->coverAttempts = 0;
      book->coverRetryAfterMs = 0;
    }
    if (!book->coverPixels)
      continue;
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
    book->coverWidth = 0;
    book->coverHeight = 0;
  }
}

void LibraryController::LoadVisibleBrowserCoverCaches() {
  if (app_.BookCount() <= 0)
    return;

  // Filename/date sorting does not need metadata for hidden books. Populate
  // only visible labels, including list mode where covers are disabled.
  if (app_.prefs &&
      (app_.prefs->library_sort_mode == LIBRARY_SORT_FILENAME ||
       app_.prefs->library_sort_mode == LIBRARY_SORT_DATE_MODIFIED)) {
    const browser_cover_cache_utils::VisibleRange metadata_visible =
        browser_cover_cache_utils::ComputeVisibleRange(
            app_.GetBrowserPageStart(), app_.BookCount(),
            CurrentBrowserPageSize(app_));
    for (int i = metadata_visible.start; i < metadata_visible.end; ++i) {
      Book *book = app_.books[i];
      if (book && !book->IsBrowserFolder() &&
          app_flow_utils::SupportsMetadataIndexing(book->format))
        book->TryLoadMetadataFromCache();
    }
  }

  if (!ShouldCurrentBrowserLoadCovers(app_)) {
    UnloadNonVisibleBrowserCoverCaches();
    return;
  }

  UnloadNonVisibleBrowserCoverCaches();
  const browser_cover_cache_utils::VisibleRange visible =
      browser_cover_cache_utils::ComputeVisibleRange(
          app_.GetBrowserPageStart(), app_.BookCount(),
          CurrentBrowserPageSize(app_));
  const int start = visible.start;
  const int end = visible.end;
  for (int i = start; i < end; i++) {
    Book *book = app_.books[i];
    if (!book || book->coverPixels)
      continue;

    std::string path = BuildBookPath(book);
    if (path.empty())
      continue;
    if (cover_cache::TryLoad(book, path)) {
      book->coverAttempts = kCoverMaxAttempts;
      book->coverRetryAfterMs = 0;
    }
  }
}
