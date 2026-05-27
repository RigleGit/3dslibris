/*
    3dslibris - app_browser_jobs.cpp
    Extracted from app_browser.cpp. Owns the background job queue used
    to warm the browser grid: metadata indexing, cover extraction, and
    TOC resolution. ProcessJobs is the time-budgeted pump driven from
    the main loop during idle periods.

    No behavior change — pure code motion. Same anonymous-namespace
    helper duplication as in app_browser_covers.cpp; consolidating to
    a private header is a follow-up.
*/

#include "app/app.h"
#include "app/library_controller.h"

#include <algorithm>
#include <deque>
#include <stdint.h>
#include <stdio.h>
#include <string>

#include <3ds.h>

#include "book/book.h"
#include "book/book_parser.h"
#include "formats/cbz/cbz_parser.h"
#include "formats/common/book_error.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_job_queue_utils.h"
#include "library/browser_view_utils.h"
#include "library/browser_warmup_utils.h"
#include "library/cover_cache.h"
#include "menus/chapter_menu.h"
#include "settings/prefs.h"
#include "shared/app_flow_utils.h"
#include "shared/debug_log.h"
#include "shared/debug_runtime_mode.h"
#include "shared/home_button_guard.h"
#include "shared/string_utils.h"
#include "ui/text.h"

#ifndef BROWSER_COVER_TRACE
#define BROWSER_COVER_TRACE 0
#endif

#ifndef BROWSER_JOB_TRACE
#define BROWSER_JOB_TRACE 0
#endif

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

static bool SupportsBrowserCoverWarmup(const App &app, format_t format,
                                       const char *filename) {
  if (format == FORMAT_EPUB || format == FORMAT_CBZ)
    return true;
  if (format == FORMAT_PDF)
    return browser_warmup_utils::ShouldAttemptPdfCoverWarmup(
        app.IsNew3dsDevice());
  if (format != FORMAT_XHTML || !filename)
    return false;
  return HasExtCI(filename, ".fb2") || HasExtCI(filename, ".mobi") ||
         HasExtCI(filename, ".txt") || HasExtCI(filename, ".md") ||
         HasExtCI(filename, ".markdown") || HasExtCI(filename, ".rtf") ||
         HasExtCI(filename, ".odt");
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

bool LibraryController::HasQueuedJob(app_job_type_t type, Book *book) const {
  for (const auto &job : job_queue_) {
    if (job.type == type && job.book == book)
      return true;
  }
  return false;
}

void LibraryController::PrioritizeSelectedBookJobs(Book *selected_book) {
  if (!selected_book || selected_book->IsBrowserFolder() || job_queue_.empty())
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
  if (!book || book->IsBrowserFolder())
    return;
  if (HasQueuedJob(type, book))
    return;
  app_job_t job;
  job.type = type;
  job.book = book;
  job_queue_.push_back(job);
}

void LibraryController::QueueBookWarmup(Book *book) {
  if (!book || book->IsBrowserFolder())
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
#ifdef DSLIBRIS_DEBUG
  const size_t queue_before = job_queue_.size();
#endif
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
    if (supports_cover && should_load_covers && should_queue_cover)
      EnqueueJob(APP_JOB_EXTRACT_COVER, book);
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
  if (selected && !selected->IsBrowserFolder()) {
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
    if (!book || book->IsBrowserFolder() || book == app_.GetSelectedBook())
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
  if (!book || book->IsBrowserFolder() || book->format != FORMAT_EPUB ||
      book->tocResolveTried)
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
          if (!candidate.book || candidate.book->IsBrowserFolder())
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
    if (!book || book->IsBrowserFolder())
      continue;
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
        HomeButtonGuard home_guard;
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

        if (cover_cache::TryLoadAdjacentOverride(book, path)) {
          cover_cache::Save(book, path);
          book->coverAttempts = kCoverMaxAttempts;
          book->coverRetryAfterMs = 0;
          rc = 0;
        } else
        if (book->format == FORMAT_EPUB) {
          if (!book->metadataIndexTried) {
            // Metadata not yet attempted; queue it first and retry cover after.
            EnqueueJob(APP_JOB_INDEX_METADATA, book);
            EnqueueJob(APP_JOB_EXTRACT_COVER, book);
          } else {
            if (!book->coverImagePath.empty()) {
              HomeButtonGuard home_guard;
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
          HomeButtonGuard home_guard;
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
          HomeButtonGuard home_guard;
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
          HomeButtonGuard home_guard;
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
          HomeButtonGuard home_guard;
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
        } else {
          // Formats like TXT/MD/RTF/ODT have no embedded-cover extractor.
          // If no adjacent override was found, stop retrying.
          book->coverAttempts = kCoverMaxAttempts;
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
        HomeButtonGuard home_guard;
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
