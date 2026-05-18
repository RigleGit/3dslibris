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
  if (!book || book->IsBrowserFolder() || !book->GetFolderName() ||
      !book->GetFileName())
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
    for (int i = 0; i < app_.BookCount(); i++) {
      Book *book = app_.books[i];
      if (!book || book->IsBrowserFolder() || !book->coverPixels)
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
    if (!book || book->IsBrowserFolder() || !book->coverPixels)
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
    if (!book || book->IsBrowserFolder() || book->coverPixels)
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
