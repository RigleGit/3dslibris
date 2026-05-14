/*
    3dslibris - app_browser.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Native FS-based book discovery and UTF-8 normalization hardening.
    - Browser cover cache/preload integration and deferred metadata jobs.
    - Touch navigation and footer interactions adapted to 3DS orientation.
*/

#include "app/app.h"
#include "app/library_controller.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#include <3ds.h>

#include "book/book.h"
#include "book/book_parser.h"
#include "ui/browser_nav.h"
#include "formats/common/book_error.h"
#include "ui/button.h"
#include "menus/chapter_menu.h"
#include "shared/debug_log.h"
#include "formats/cbz/cbz_parser.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "parse.h"
#include "shared/app_flow_utils.h"
#include "shared/color_utils.h"
#include "library/browser_cover_cache_utils.h"
#include "library/cover_cache.h"
#include "library/browser_grid_view.h"
#include "library/browser_job_queue_utils.h"
#include "library/browser_list_view.h"
#include "library/browser_presentation_utils.h"
#include "library/browser_view_utils.h"
#include "library/browser_warmup_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/string_utils.h"
#include "settings/prefs.h"
#include "shared/path_utils.h"
#include "ui/text.h"
#include "shared/utf8_utils.h"
#include "app/version.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

#ifndef BROWSER_COVER_TRACE
#define BROWSER_COVER_TRACE 0
#endif

#ifndef BROWSER_JOB_TRACE
#define BROWSER_JOB_TRACE 0
#endif

namespace {

// Max extraction attempts per session before a book is skipped.
// Two retries absorb transient SD-card read failures without retrying forever.
static const uint8_t kCoverMaxAttempts = cover_cache::kMaxAttempts;
static const int kBrowserFooterY = 296;

static BrowserViewMode CurrentBrowserViewMode(const App &app) {
  if (!app.prefs)
    return BROWSER_VIEW_GALLERY;
  return app.prefs->browser_view_mode;
}

static int CurrentBrowserPageSize(const App &app) {
  return browser_view_utils::PageSize(CurrentBrowserViewMode(app));
}

static int CurrentBrowserColumnCount(const App &app) {
  return browser_view_utils::ColumnCount(CurrentBrowserViewMode(app));
}

static bool ShouldCurrentBrowserLoadCovers(const App &app) {
  return browser_view_utils::ShouldLoadCovers(CurrentBrowserViewMode(app));
}

static bool SupportsBrowserCoverWarmup(const App &app, format_t format,
                                       const char *filename) {
  if (format == FORMAT_EPUB || format == FORMAT_CBZ)
    return true;
  if (format == FORMAT_PDF)
    return browser_warmup_utils::ShouldAttemptPdfCoverWarmup(
        app.IsNew3dsDevice());
  if (format != FORMAT_XHTML || !filename)
    return false;
  return HasExtCI(filename, ".fb2") || HasExtCI(filename, ".mobi");
}

static size_t CountQueuedHeavyJobs(const std::deque<app_job_t> &jobs) {
  size_t count = 0;
  for (const auto &job : jobs) {
    if (browser_job_queue_utils::IsHeavyBrowserJobType(
            job.type, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER)) {
      count++;
    }
  }
  return count;
}

static void LayoutBrowserNavButtons(App *app) {
  app->buttonprev.Move(2, kBrowserFooterY);
  app->buttonprev.Resize(66, 22);
  app->buttonprev.Label("prev");

  app->buttonnext.Move(172, kBrowserFooterY);
  app->buttonnext.Resize(66, 22);
  app->buttonnext.Label("next");

  app->buttonprefs.Move(72, kBrowserFooterY);
  app->buttonprefs.Resize(96, 22);
  app->buttonprefs.Label("settings");
}

static std::string BuildBookPath(Book *book) {
  if (!book || !book->GetFolderName() || !book->GetFileName())
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
      if (!book || !book->coverPixels)
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
    if (!book || !book->coverPixels)
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

namespace {

static BrowserGridMarqueeState g_marquee;

} // namespace

void LibraryController::ResetBrowserMarquee() { g_marquee.Reset(); }

bool LibraryController::HasQueuedJob(app_job_type_t type, Book *book) const {
  for (const auto &job : job_queue_) {
    if (job.type == type && job.book == book)
      return true;
  }
  return false;
}

void LibraryController::PrioritizeSelectedBookJobs(Book *selected_book) {
  if (!selected_book || job_queue_.empty())
    return;

  // Move the selected book's pending jobs to the front so they run first,
  // but keep all other books' jobs in the queue — they will be processed
  // during subsequent idle periods rather than being discarded.
  std::deque<app_job_t> selected_jobs;
  std::deque<app_job_t> other_jobs;
  for (size_t i = 0; i < job_queue_.size(); i++) {
    if (job_queue_[i].book == selected_book)
      selected_jobs.push_back(job_queue_[i]);
    else
      other_jobs.push_back(job_queue_[i]);
  }
  job_queue_.clear();
  for (size_t i = 0; i < selected_jobs.size(); i++)
    job_queue_.push_back(selected_jobs[i]);
  for (size_t i = 0; i < other_jobs.size(); i++)
    job_queue_.push_back(other_jobs[i]);

#ifdef DSLIBRIS_DEBUG
  if (!selected_jobs.empty()) {
    DBG_LOGF(&app_,
             "BROWSER: prioritized selected book jobs=%u queue=%u book=%s",
             (unsigned)selected_jobs.size(), (unsigned)job_queue_.size(),
             selected_book->GetFileName() ? selected_book->GetFileName()
                                          : "(null)");
  }
#endif
}

void LibraryController::EnqueueJob(app_job_type_t type, Book *book) {
  if (!book)
    return;
  if (HasQueuedJob(type, book))
    return;
  app_job_t job;
  job.type = type;
  job.book = book;
  job_queue_.push_back(job);
}

void LibraryController::QueueBookWarmup(Book *book) {
  if (!book)
    return;
  const format_t format =
      app_flow_utils::DetectBookFormat(book->GetFileName());
  const bool supports_metadata =
      app_flow_utils::SupportsMetadataIndexing(format);
  const bool supports_cover =
      SupportsBrowserCoverWarmup(app_, book->format, book->GetFileName());
  const bool metadata_done =
      browser_warmup_utils::MetadataWarmupDone(supports_metadata,
                                               book->metadataIndexTried);
  const bool cover_done = browser_warmup_utils::CoverWarmupDone(
      supports_cover, book->coverPixels != nullptr, book->coverAttempts,
      kCoverMaxAttempts);
  const u64 now_ms = osGetTime();
  const bool cover_retry_pending =
      !cover_done && book->coverRetryAfterMs != 0 &&
      now_ms < book->coverRetryAfterMs;
  if (metadata_done && cover_done) {
    return;
  }
  if (cover_retry_pending && metadata_done)
    return;
  const size_t queue_before = job_queue_.size();
  const size_t queued_heavy_jobs = CountQueuedHeavyJobs(job_queue_);
  const bool is_selected_book = (book == app_.GetSelectedBook());
  const bool warmup_idle = browser_warmup_utils::IsBrowserWarmupIdle(
      now_ms, app_.GetBrowserLastInteractionMs(),
      app_.IsBrowserWaitingInputRelease());
  const bool heavy_idle = browser_warmup_utils::IsBrowserHeavyWarmupIdleForDevice(
      app_.IsNew3dsDevice(),
      now_ms, app_.GetBrowserLastInteractionMs(),
      app_.IsBrowserWaitingInputRelease());
  const bool should_queue_cover =
      browser_warmup_utils::ShouldQueueCoverWarmupForDevice(
          app_.IsNew3dsDevice(), is_selected_book, warmup_idle, heavy_idle,
          queued_heavy_jobs);
  const bool should_load_covers = ShouldCurrentBrowserLoadCovers(app_);

  if (book->format == FORMAT_EPUB) {
    if (!book->metadataIndexTried &&
        app_flow_utils::SupportsMetadataIndexing(format))
      EnqueueJob(APP_JOB_INDEX_METADATA, book);
    if (should_load_covers && should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_PDF) {
    if (!book->metadataIndexTried &&
        app_flow_utils::SupportsMetadataIndexing(format))
      EnqueueJob(APP_JOB_INDEX_METADATA, book);
    if (supports_cover && should_load_covers && should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_CBZ) {
    if (should_load_covers && should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
  } else if (book->format == FORMAT_XHTML) {
    if (HasExtCI(book->GetFileName(), ".fb2") ||
        HasExtCI(book->GetFileName(), ".mobi")) {
      if (should_load_covers && should_queue_cover)
        EnqueueJob(APP_JOB_EXTRACT_COVER, book);
    }
  }
#ifdef DSLIBRIS_DEBUG
  if (job_queue_.size() != queue_before) {
    DBG_LOGF(&app_,
             "BROWSER: warmup queued added=%u queue=%u book=%s format=%d "
             "selected=%u warmup_idle=%u heavy_idle=%u cover=%u",
             (unsigned)(job_queue_.size() - queue_before),
             (unsigned)job_queue_.size(),
             book->GetFileName() ? book->GetFileName() : "(null)",
             (int)book->format, is_selected_book ? 1u : 0u,
             warmup_idle ? 1u : 0u, heavy_idle ? 1u : 0u,
             should_queue_cover ? 1u : 0u);
  }
#endif
}

void LibraryController::TickBrowserWarmup() {
  if (debug_runtime::BrowserWarmupDisabled())
    return;
  if (app_.IsAppletSuspended() || app_.GetMode() != AppMode::Browser ||
      !app_.GetSelectedBook())
    return;
  if (!browser_warmup_utils::IsBrowserWarmupIdle(
          osGetTime(), app_.GetBrowserLastInteractionMs(),
          app_.IsBrowserWaitingInputRelease())) {
    return;
  }
  Book *selected = app_.GetSelectedBook();
  if (selected) {
    const format_t selected_format =
        app_flow_utils::DetectBookFormat(selected->GetFileName());
    const bool selected_supports_metadata =
        app_flow_utils::SupportsMetadataIndexing(selected_format);
    const bool selected_supports_cover =
        SupportsBrowserCoverWarmup(app_, selected->format,
                                   selected->GetFileName());
    if (!browser_warmup_utils::BookWarmupDone(
            selected_supports_metadata, selected->metadataIndexTried,
            selected_supports_cover, selected->coverPixels != nullptr,
            selected->coverAttempts, kCoverMaxAttempts)) {
      QueueBookWarmup(selected);
    }
  }
  const int page_start = app_.GetBrowserPageStart();
  const int visible_count = browser_warmup_utils::VisibleBrowserEntryCount(
      CurrentBrowserPageSize(app_), page_start, app_.BookCount());
  for (int i = page_start;
       i < page_start + visible_count;
       i++) {
    Book *book = app_.books[i];
    if (!book || book == app_.GetSelectedBook())
      continue;
    const format_t format = app_flow_utils::DetectBookFormat(book->GetFileName());
    const bool supports_metadata =
        app_flow_utils::SupportsMetadataIndexing(format);
    const bool supports_cover =
        SupportsBrowserCoverWarmup(app_, book->format, book->GetFileName());
    if (browser_warmup_utils::BookWarmupDone(
            supports_metadata, book->metadataIndexTried, supports_cover,
            book->coverPixels != nullptr, book->coverAttempts,
            kCoverMaxAttempts)) {
      continue;
    }
    QueueBookWarmup(book);
  }
}

void LibraryController::QueueTocResolve(Book *book) {
  if (!book || book->format != FORMAT_EPUB || book->tocResolveTried)
    return;
  EnqueueJob(APP_JOB_RESOLVE_TOC, book);
}

void LibraryController::ProcessJobs(u32 budget_ms) {
  if (debug_runtime::BrowserWarmupDisabled())
    return;
  if (app_.IsAppletSuspended())
    return;
  if (job_queue_.empty())
    return;

#if defined(DSLIBRIS_DEBUG) && BROWSER_JOB_TRACE
  auto job_name = [](app_job_type_t t) -> const char * {
    switch (t) {
    case APP_JOB_INDEX_METADATA:
      return "index";
    case APP_JOB_EXTRACT_COVER:
      return "cover";
    case APP_JOB_RESOLVE_TOC:
      return "toc";
    default:
      return "unknown";
    }
  };
#endif

  u64 start_ms = osGetTime();
  while (!job_queue_.empty()) {
    if (app_.ShouldAbortWork())
      break;
    while (!job_queue_.empty() && !job_queue_.front().book)
      job_queue_.pop_front();
    if (job_queue_.empty())
      break;

    const bool allow_selected_browser_jobs =
        app_.GetMode() != AppMode::Browser ||
        browser_warmup_utils::IsBrowserWarmupIdle(
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
    const bool allow_heavy_browser_jobs =
        app_.GetMode() != AppMode::Browser ||
        browser_warmup_utils::IsBrowserHeavyWarmupIdleForDevice(
            app_.IsNew3dsDevice(),
            osGetTime(), app_.GetBrowserLastInteractionMs(),
            app_.IsBrowserWaitingInputRelease());
    app_job_t job = {};
    const bool got_job = browser_job_queue_utils::TakeFirstAllowedJob(
        &job_queue_, &job, [&](const app_job_t &candidate) {
          if (!candidate.book)
            return false;
          if (!browser_job_queue_utils::IsHeavyBrowserJobType(
                  candidate.type, APP_JOB_INDEX_METADATA,
                  APP_JOB_EXTRACT_COVER)) {
            return true;
          }
          if (app_.GetMode() != AppMode::Browser)
            return true;
          if (candidate.book == app_.GetSelectedBook())
            return allow_selected_browser_jobs;
          return allow_heavy_browser_jobs;
        });
    if (!got_job)
      break;


    Book *book = job.book;
    int rc = 0;
#if defined(DSLIBRIS_DEBUG) && BROWSER_JOB_TRACE
    u64 t0 = osGetTime();
#endif

    if (job.type == APP_JOB_EXTRACT_COVER && app_.GetMode() == AppMode::Browser &&
        !ShouldCurrentBrowserLoadCovers(app_)) {
      continue;
    }

    if (job.type == APP_JOB_INDEX_METADATA) {
      if (!book->metadataIndexTried &&
          app_flow_utils::SupportsMetadataIndexing(
              app_flow_utils::DetectBookFormat(book->GetFileName()))) {
        rc = book_parser::Index(book);
        if (rc == 0) {
          book->ClearBrowserDisplayNameCache();
          if (book == app_.GetSelectedBook())
            ResetBrowserMarquee();
        }
        app_.SetBrowserDirty(true);
      }
    } else if (job.type == APP_JOB_EXTRACT_COVER) {
      if (!book->coverPixels && book->coverAttempts < kCoverMaxAttempts) {
        std::string path = BuildBookPath(book);
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
        const bool had_cover_pixels = (book->coverPixels != nullptr);
        const uint8_t cover_attempts_before = book->coverAttempts;
#endif
        if (path.empty()) {
          rc = 1;
          book->coverAttempts = kCoverMaxAttempts; // path failure is permanent
        } else {
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
        DBG_LOGF(&app_, "COVER: extract start book=%s format=%d attempt=%u",
                 book->GetFileName() ? book->GetFileName() : "(null)",
                 (int)book->format, (unsigned)book->coverAttempts);
#endif
        const bool is_selected_book = (book == app_.GetSelectedBook());
        const u64 free_bytes = (u64)osGetMemRegionFree(MEMREGION_ALL);
        if (!browser_warmup_utils::HasCoverExtractionHeadroom(
                app_.IsNew3dsDevice(), is_selected_book, free_bytes)) {
          book->coverAttempts++;
          const u64 retry_delay_ms = browser_warmup_utils::CoverRetryDelayMs(
              app_.IsNew3dsDevice(), is_selected_book, 4, false);
          if (retry_delay_ms != 0 && book->coverAttempts < kCoverMaxAttempts)
            book->coverRetryAfterMs = osGetTime() + retry_delay_ms;
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
          DBG_LOGF(&app_,
                   "COVER: skip mem-pressure book=%s selected=%u free=%llu attempts=%u",
                   book->GetFileName() ? book->GetFileName() : "(null)",
                   is_selected_book ? 1u : 0u,
                   (unsigned long long)free_bytes,
                   (unsigned)book->coverAttempts);
#endif
          continue;
        }
        if (book->format == FORMAT_EPUB) {
          if (!book->metadataIndexTried) {
            // Metadata not yet attempted; queue it first and retry cover after.
            EnqueueJob(APP_JOB_INDEX_METADATA, book);
            EnqueueJob(APP_JOB_EXTRACT_COVER, book);
          } else {
            if (!book->coverImagePath.empty()) {
              rc = epub_parser::ExtractCover(book, path);
              if (rc == 0 && book->coverPixels) {
                cover_cache::Save(book, path);
                book->coverAttempts = kCoverMaxAttempts;
                book->coverRetryAfterMs = 0;
              } else if (rc == BOOK_ERR_CANCELLED) {
              } else if (rc != 0) {
                book->coverAttempts++;
              } else {
                book->coverAttempts = kCoverMaxAttempts;
              }
            } else {
              book->coverAttempts = kCoverMaxAttempts;
            }
          }
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".fb2")) {
          rc = fb2_parser::ExtractCover(book, path);
          if (rc == 0 && book->coverPixels) {
            cover_cache::Save(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc == BOOK_ERR_CANCELLED) {
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
        } else if (book->format == FORMAT_XHTML &&
                   HasExtCI(book->GetFileName(), ".mobi")) {
          rc = mobi_parser::ExtractCover(book, path);
          if (rc == 0 && book->coverPixels) {
            cover_cache::Save(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc == BOOK_ERR_CANCELLED) {
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
        } else if (book->format == FORMAT_PDF) {
          rc = pdf_parser::ExtractCover(book, path);
          if (rc == 0 && book->coverPixels) {
            cover_cache::Save(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc == BOOK_ERR_CANCELLED) {
          } else if (rc != 0) {
            if (browser_warmup_utils::IsPermanentCoverFailure(rc))
              book->coverAttempts = kCoverMaxAttempts;
            else
              book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
        } else if (book->format == FORMAT_CBZ) {
          rc = cbz_parser::ExtractCover(book, path);
          if (rc == 0 && book->coverPixels) {
            cover_cache::Save(book, path);
            book->coverAttempts = kCoverMaxAttempts;
            book->coverRetryAfterMs = 0;
          } else if (rc == BOOK_ERR_CANCELLED) {
          } else if (rc != 0) {
            book->coverAttempts++;
          } else {
            book->coverAttempts = kCoverMaxAttempts;
          }
        }
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
        const bool has_cover_pixels = (book->coverPixels != nullptr);
        const bool cover_visual_changed =
            (has_cover_pixels != had_cover_pixels) ||
            (!has_cover_pixels && cover_attempts_before < kCoverMaxAttempts &&
             book->coverAttempts >= kCoverMaxAttempts);
#endif
        if (app_.GetMode() == AppMode::Browser) {
          const browser_cover_cache_utils::VisibleRange visible =
              browser_cover_cache_utils::ComputeVisibleRange(
                  app_.GetBrowserPageStart(), app_.BookCount(),
                  CurrentBrowserPageSize(app_));
          const int book_index = app_.GetBookIndex(book);
          if (browser_cover_cache_utils::VisibleBookNeedsBrowserRedraw(
                  visible, book_index)) {
            ResetBrowserMarquee();
            app_.ts->MarkAllScreensDirty();
            app_.SetBrowserDirty(true);
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
            DBG_LOGF(&app_,
                     "BROWSER: cover job redraw book=%s index=%d rc=%d attempts=%u "
                     "pixels=%u visual_changed=%u",
                     book->GetFileName() ? book->GetFileName() : "(null)",
                     book_index, rc, (unsigned)book->coverAttempts,
                     book->coverPixels ? 1u : 0u,
                     cover_visual_changed ? 1u : 0u);
#endif
          }
        }
        const u64 retry_delay_ms = browser_warmup_utils::CoverRetryDelayMs(
            app_.IsNew3dsDevice(), is_selected_book, rc,
            book->coverPixels != nullptr);
        if (rc != BOOK_ERR_CANCELLED && retry_delay_ms != 0 &&
            book->coverAttempts < kCoverMaxAttempts) {
          book->coverRetryAfterMs = osGetTime() + retry_delay_ms;
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
          DBG_LOGF(&app_,
                   "COVER: retry deferred book=%s rc=%d delay_ms=%llu attempt=%u",
                   book->GetFileName() ? book->GetFileName() : "(null)", rc,
                   (unsigned long long)retry_delay_ms,
                   (unsigned)book->coverAttempts);
#endif
        }
#if defined(DSLIBRIS_DEBUG) && BROWSER_COVER_TRACE
        DBG_LOGF(&app_, "COVER: extract end rc=%d book=%s pixels=%u attempts=%u",
                 rc, book->GetFileName() ? book->GetFileName() : "(null)",
                 book->coverPixels ? 1u : 0u, (unsigned)book->coverAttempts);
#endif
        }
      }
    } else if (job.type == APP_JOB_RESOLVE_TOC) {
      if (book->format == FORMAT_EPUB && !book->tocResolveTried) {
        std::string path = BuildBookPath(book);
        if (path.empty())
          continue;
        rc = epub_parser::ResolveToc(book, path);
        book->tocResolveTried = true;
        book->tocResolved = (rc == 0);
        if (rc == 0 && app_.GetMode() == AppMode::Chapters &&
            book == app_.GetCurrentBook() && app_.chaptermenu) {
          app_.chaptermenu->Init();
          app_.chaptermenu->SetDirty(true);
        }
      }
    }

#if defined(DSLIBRIS_DEBUG) && BROWSER_JOB_TRACE
    {
      u64 elapsed = osGetTime() - t0;
      char msg[256];
      if (const char *tag = BookOpenErrorTag(rc)) {
        snprintf(msg, sizeof(msg), "TIMING: job=%s rc=%s ms=%llums book=%s",
                 job_name(job.type), tag, (unsigned long long)elapsed,
                 book->GetFileName() ? book->GetFileName() : "(null)");
      } else {
        snprintf(msg, sizeof(msg), "TIMING: job=%s rc=%d ms=%llums book=%s",
                 job_name(job.type), rc, (unsigned long long)elapsed,
                 book->GetFileName() ? book->GetFileName() : "(null)");
      }
      DBG_LOG(&app_, msg);
    }
#endif

    if (app_.ShouldAbortWork())
      break;

    if (osGetTime() - start_ms >= budget_ms)
      break;
  }
}

size_t LibraryController::PauseBrowserJobs() {
  std::deque<app_job_t> kept;
  size_t removed = 0;
  while (!job_queue_.empty()) {
    const app_job_t job = job_queue_.front();
    job_queue_.pop_front();
    if (browser_job_queue_utils::IsHeavyBrowserJobType(
            job.type, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER)) {
      removed++;
      continue;
    }
    kept.push_back(job);
  }
  job_queue_.swap(kept);
  return removed;
}

void LibraryController::browser_handleevent() {
  if (app_.ShouldAbortWork())
    return;

  // Re-apply browser layout in case another view reused/moved shared buttons.
  LayoutBrowserNavButtons(&app_);

  u32 keys = hidKeysDown();
#ifdef DSLIBRIS_DEBUG
  if (keys) {
    DBG_LOGF(&app_, "BROWSER handleevent keys=0x%08lx",
             (unsigned long)keys);
  }
#endif
  auto map_grid_nav = [&](u32 key_down, BrowserNavMove *move) -> bool {
    const u32 nav_down = app_.key.ddown | app_.key.down;
    const u32 nav_up = app_.key.dup | app_.key.up;
    const u32 nav_left = app_.key.dleft | app_.key.left;
    const u32 nav_right = app_.key.dright | app_.key.right;
    if (!move)
      return false;
    if (!app_.orientation) {
      // Turned Left (right-handed): rotate d-pad mapping so directional input
      // follows the visual page orientation.
      if (key_down & nav_down) {
        *move = BROWSER_NAV_RIGHT;
        return true;
      }
      if (key_down & nav_up) {
        *move = BROWSER_NAV_LEFT;
        return true;
      }
      if (key_down & nav_left) {
        *move = BROWSER_NAV_DOWN;
        return true;
      }
      if (key_down & nav_right) {
        *move = BROWSER_NAV_UP;
        return true;
      }
      return false;
    }

    // Turned Right (left-handed): rotate d-pad mapping so directional intent
    // matches the on-screen grid orientation.
    if (key_down & nav_down) {
      *move = BROWSER_NAV_LEFT;
      return true;
    }
    if (key_down & nav_up) {
      *move = BROWSER_NAV_RIGHT;
      return true;
    }
    if (key_down & nav_left) {
      *move = BROWSER_NAV_UP;
      return true;
    }
    if (key_down & nav_right) {
      *move = BROWSER_NAV_DOWN;
      return true;
    }
    return false;
  };
  const u32 release_mask = KEY_TOUCH | app_.key.a | app_.key.b | app_.key.x |
                           app_.key.y | app_.key.start | app_.key.select |
                           app_.key.dup | app_.key.ddown | app_.key.dleft |
                           app_.key.dright | app_.key.up | app_.key.down |
                           app_.key.left | app_.key.right | app_.key.l |
                           app_.key.r | app_.key.zl | app_.key.zr;
  if (app_.IsBrowserWaitingInputRelease()) {
    const u64 now = osGetTime();
    if (hidKeysHeld() & release_mask &&
        !browser_warmup_utils::ShouldForceClearInputRelease(
            now, app_.GetBrowserLastInteractionMs(), true))
      return;
    app_.SetBrowserWaitingInputRelease(false);
    if (browser_warmup_utils::ShouldForceClearInputRelease(
            now, app_.GetBrowserLastInteractionMs(), true))
      app_.SetBrowserLastInteractionMs(now);
    return;
  }

  auto navigateSelection = [&](BrowserNavMove move) {
    if (app_.BookCount() <= 0)
      return;
    const int old_page_start = app_.GetBrowserPageStart();
    Book *old_selected = app_.GetSelectedBook();
    const int page_size = CurrentBrowserPageSize(app_);
    const int columns = CurrentBrowserColumnCount(app_);
    const int old_index = app_.GetBookIndex(app_.GetSelectedBook());
    if (old_index < 0)
      return;
    const int old_index_on_page = old_index - old_page_start;
    const int books_remaining = app_.BookCount() - old_page_start;
    const int visible_on_page =
        (books_remaining < page_size) ? books_remaining : page_size;

    // At the edges of the current page, overflow d-pad navigation into a page
    // flip rather than clamping in place.
    if (columns == 1) {
      // List mode: top item + backward → prev page; bottom item + forward → next page.
      if (move == BROWSER_NAV_LEFT && old_index_on_page == 0) {
        browser_prevpage();
        return;
      }
      if (move == BROWSER_NAV_RIGHT && old_index_on_page == visible_on_page - 1) {
        browser_nextpage();
        return;
      }
    } else {
      // Gallery mode: left-column item + left → prev page; right-column item + right → next page.
      if (move == BROWSER_NAV_LEFT && old_index_on_page % columns == 0) {
        browser_prevpage();
        return;
      }
      if (move == BROWSER_NAV_RIGHT &&
          old_index_on_page % columns == columns - 1) {
        browser_nextpage();
        return;
      }
    }

    BrowserNavState state = {old_index, old_page_start};
    state = BrowserNavMoveSelection(state, app_.BookCount(), page_size, columns,
                                    move);
    if (state.selected_index < 0 || state.selected_index >= app_.BookCount())
      return;
    app_.SetBrowserPageStart(state.page_start);
    app_.SetSelectedBook(app_.books[state.selected_index]);
    if (app_.GetBrowserPageStart() != old_page_start)
      LoadVisibleBrowserCoverCaches();
    if (app_.GetSelectedBook() != old_selected) {
      g_marquee.Reset();
      PrioritizeSelectedBookJobs(app_.GetSelectedBook());
      app_.SetBrowserLastInteractionMs(osGetTime());
    }
    app_.SetBrowserDirty(true);
  };

  BrowserNavMove nav_move = BROWSER_NAV_LEFT;
  const bool has_grid_nav = map_grid_nav(keys, &nav_move);

  if (keys & app_.key.a) {
    app_.OpenBook();
  } else if (keys & app_.key.start) {
    app_.SetMode(AppMode::Quit);
  } else if (has_grid_nav) {
    navigateSelection(nav_move);
  } else if (keys & app_.key.l) {
    browser_prevpage();
  } else if (keys & app_.key.r) {
    browser_nextpage();
  }

  else if (keys & app_.key.x) {
    int mode = app_.ts->GetColorMode();
    int next = (mode + 1) % 6;
    app_.colorMode = next;
    app_.ts->SetColorMode(next);
    UiButtonSkin_SetColorMode(next);
    app_.ts->MarkAllScreensDirty();
    g_marquee.Reset();
    app_.SetBrowserDirty(true);
  }

  else if (keys & (app_.key.select | app_.key.y)) {
    app_.ShowSettingsView(false);
  }

  else if (keys & KEY_TOUCH) {
    auto hitsButtonAt = [&](Button &button, int px, int py, int slack) {
      if (slack <= 0)
        return button.EnclosesPoint((u16)px, (u16)py);
      for (int dy = -slack; dy <= slack; dy += slack) {
        for (int dx = -slack; dx <= slack; dx += slack) {
          int x = px + dx;
          int y = py + dy;
          if (x < 0 || y < 0 || x > 239 || y > 319)
            continue;
          if (button.EnclosesPoint((u16)x, (u16)y))
            return true;
        }
      }
      return false;
    };

    auto handleTouchAt = [&](int x, int y) -> bool {
      if (x < 0 || y < 0 || x > 239 || y > 319)
        return false;

      if (hitsButtonAt(app_.buttonnext, x, y, 4)) {
        browser_nextpage();
        return true;
      }
      if (hitsButtonAt(app_.buttonprev, x, y, 4)) {
        browser_prevpage();
        return true;
      }
      if (hitsButtonAt(app_.buttonprefs, x, y, 4)) {
        app_.ShowSettingsView(false);
        return true;
      }

      // Prefer coarse cell hit-test (cover + title/progress area):
      // single tap selects, tapping selected book opens.
      int book_idx = -1;
      if (CurrentBrowserViewMode(app_) == BROWSER_VIEW_LIST) {
        book_idx = browser_list_view::HitTestBookIndex(
            x, y, app_.GetBrowserPageStart(), app_.BookCount(),
            CurrentBrowserPageSize(app_));
      } else {
        book_idx = browser_grid_view::HitTestBookIndex(
            x, y, app_.GetBrowserPageStart(), app_.BookCount());
        if (book_idx < 0) {
          for (int i = app_.GetBrowserPageStart();
               (i < app_.BookCount()) &&
               (i < app_.GetBrowserPageStart() + APP_BROWSER_BUTTON_COUNT);
               i++) {
            if (hitsButtonAt(*app_.buttons[i], x, y, 4)) {
              book_idx = i;
              break;
            }
          }
        }
      }
      if (book_idx >= 0 && book_idx < app_.BookCount()) {
        if (app_.GetSelectedBook() == app_.books[book_idx]) {
          app_.OpenBook();
        } else {
          app_.SetSelectedBook(app_.books[book_idx]);
          g_marquee.Reset();
          PrioritizeSelectedBookJobs(app_.GetSelectedBook());
          app_.SetBrowserLastInteractionMs(osGetTime());
          app_.SetBrowserDirty(true);
        }
        return true;
      }
      return false;
    };

    touchPosition mapped = app_.TouchRead();
    handleTouchAt((int)mapped.px, (int)mapped.py);
  }
}

void LibraryController::browser_init(void) {
  for (int i = 0; i < app_.BookCount(); i++) {
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % browser_grid_view::kGridCols;
    int row = page_idx / browser_grid_view::kGridCols;

    app_.buttons.push_back(new Button());
    app_.buttons[i]->Init(app_.ts.get());
    app_.buttons[i]->Resize(browser_grid_view::kCoverW + 4,
                            browser_grid_view::kCoverH + 4);
    app_.buttons[i]->Move(browser_grid_view::kGridX0 +
                              col * browser_grid_view::kCellW,
                          browser_grid_view::kGridY0 +
                              row * browser_grid_view::kCellH);

    // Cover extraction moved to browser_draw to avoid freezing at startup
    app_.buttons[i]->SetLabel1(std::string(""));

  }

  app_.buttonprev.Init(app_.ts.get());
  app_.buttonnext.Init(app_.ts.get());
  app_.buttonprefs.Init(app_.ts.get());
  LayoutBrowserNavButtons(&app_);

  if (app_.BookCount() <= 0) {
    app_.SetBrowserPageStart(0);
    app_.SetSelectedBook(NULL);
    app_.SetBrowserLastInteractionMs(osGetTime());
    app_.SetBrowserDirty(true);
    return;
  }

  if (!app_.GetSelectedBook()) {
    app_.SetBrowserPageStart(0);
    app_.SetSelectedBook(app_.books[0]);
  } else {
    app_.SetBrowserPageStart(
        (app_.GetBookIndex(app_.GetSelectedBook()) / CurrentBrowserPageSize(app_)) *
        CurrentBrowserPageSize(app_));
  }
  g_marquee.Reset();
  PrioritizeSelectedBookJobs(app_.GetSelectedBook());
  app_.SetBrowserLastInteractionMs(osGetTime());
  LoadVisibleBrowserCoverCaches();
}

void LibraryController::browser_nextpage() {
  const int page_size = CurrentBrowserPageSize(app_);
  if (app_.GetBrowserPageStart() + page_size < app_.BookCount()) {
    app_.SetBrowserPageStart(app_.GetBrowserPageStart() + page_size);
    app_.SetSelectedBook(app_.books[app_.GetBrowserPageStart()]);
    g_marquee.Reset();
    PrioritizeSelectedBookJobs(app_.GetSelectedBook());
    app_.SetBrowserLastInteractionMs(osGetTime());
    LoadVisibleBrowserCoverCaches();
    app_.SetBrowserDirty(true);
  }
}

void LibraryController::browser_prevpage() {
  const int page_size = CurrentBrowserPageSize(app_);
  if (app_.GetBrowserPageStart() >= page_size) {
    app_.SetBrowserPageStart(app_.GetBrowserPageStart() - page_size);
    app_.SetSelectedBook(
        app_.books[app_.GetBrowserPageStart() + page_size - 1]);
    g_marquee.Reset();
    PrioritizeSelectedBookJobs(app_.GetSelectedBook());
    app_.SetBrowserLastInteractionMs(osGetTime());
    LoadVisibleBrowserCoverCaches();
    app_.SetBrowserDirty(true);
  }
}

void LibraryController::browser_draw(void) {
  // Keep footer controls stable after view switches.
  LayoutBrowserNavButtons(&app_);

  // save state
  int colorMode = app_.ts->GetColorMode();
  u16 *screen = app_.ts->GetScreen();
  int style = app_.ts->GetStyle();
  int savedPixelSize = app_.ts->pixelsize;

  app_.ts->SetScreen(app_.ts->screenleft);
  app_.ts->SetStyle(TEXT_STYLE_BROWSER);
  app_.ts->PrintSplash(app_.ts->screenleft);
  {
    char versionMsg[16];
    snprintf(versionMsg, sizeof(versionMsg), "v%s", VERSION);
    const int versionWidth =
        app_.ts->GetStringWidth(versionMsg, TEXT_STYLE_BROWSER);
    int versionX = (240 - versionWidth) / 2;
    if (versionX < 0)
      versionX = 0;
    app_.ts->SetPixelSize(10);
  app_.ts->SetPen(versionX, 57);
  app_.ts->PrintString(versionMsg);
  }

  app_.ts->SetScreen(app_.ts->screenright);
  app_.ts->ClearScreen();
  app_.DrawBottomGradientBackground();

  const BrowserViewMode view_mode = CurrentBrowserViewMode(app_);
  const int page_size = CurrentBrowserPageSize(app_);

  BrowserDrawContext ctx{app_.ts.get(), &app_.books, app_.GetSelectedBook(),
                         &app_.buttons};
  if (view_mode == BROWSER_VIEW_LIST)
    browser_list_view::DrawPage(ctx, app_.GetBrowserPageStart(), page_size);
  else
    browser_grid_view::DrawPage(ctx, g_marquee, app_.GetBrowserPageStart());

  app_.ts->SetPixelSize(savedPixelSize);

  if (app_.GetBrowserPageStart() >= page_size)
    app_.buttonprev.Draw(app_.ts->screenright, false);
  if (app_.BookCount() > app_.GetBrowserPageStart() + page_size)
    app_.buttonnext.Draw(app_.ts->screenright, false);

  app_.buttonprefs.Draw(app_.ts->screenright, false);

  if (app_.BookCount() > page_size) {
    int currentPage = (app_.GetBrowserPageStart() / page_size) + 1;
    int totalPages = (app_.BookCount() + page_size - 1) / page_size;
    char pageMsg[32];
    snprintf(pageMsg, sizeof(pageMsg), "%d/%d", currentPage, totalPages);
    app_.ts->SetPixelSize(8);
    app_.ts->SetPen(112, kBrowserFooterY + 3);
    app_.ts->PrintString(pageMsg);
    app_.ts->SetPixelSize(savedPixelSize);
  }

  app_.ts->SetColorMode(colorMode);
  app_.ts->SetScreen(screen);
  app_.ts->SetStyle(style);

  app_.SetBrowserDirty(false);
}

void LibraryController::browser_tick_marquee() {
  browser_grid_view::TickMarquee(app_.ts.get(), g_marquee);
}
