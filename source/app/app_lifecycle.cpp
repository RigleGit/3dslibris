/*
    3dslibris - app_lifecycle.cpp
    Extracted from app.cpp. Holds the APT applet lifecycle path
    (PrepareForShutdown, AptHookCallback, HandleAppletHook,
    HandleAppletSuspend, HandleAppletResume).

    See the comments in HandleAppletHook for why logging and SD I/O are
    forbidden inside the hook callback — touching this file requires
    understanding the HOME menu acknowledgment timing window.

    No behavior change — pure code motion.
*/

#include "app/app.h"

#include <3ds.h>

#include "book/book.h"
#include "book/book_renderer.h"
#include "shared/debug_log.h"
#include "ui/text.h"

void App::PrepareForShutdown()
{
  if (lifecycle_state_.IsShutdownPrepared())
    return;
  lifecycle_state_.MarkShutdownPrepared();

#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "SHUTDOWN begin env=%s mode=%d current_session=%u opening_session=%u suspended=%u",
           lifecycle_state_.IsHomebrew() ? "3dsx/homebrew" : "cia/title", (int)nav_.mode,
           reader_state_.current_book_session_id, reader_state_.opening.session_id,
           lifecycle_state_.IsSuspended() ? 1u : 0u);
#endif

  pending_boot_reopen_ = false;
  skip_next_browser_present_ = false;
  lifecycle_state_.SetResumePending(false);
  lifecycle_state_.SetSuspendHandled(false);
  lifecycle_state_.SetExitRequested(false);
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  ResetPageRepeat();

  PersistPrefs();

#ifdef DSLIBRIS_DEBUG
  DBG_LOG(this, "SHUTDOWN cancel workers begin");
#endif
  const size_t removed_jobs = PauseBrowserJobs();
#ifndef DSLIBRIS_DEBUG
  (void)removed_jobs;
#endif

  Book *opening_book = reader_state_.opening.book;
  if (opening_book)
  {
    opening_book->RequestAbortOpen();
    book_renderer::CancelFixedLayoutDeferredWork(opening_book);
    opening_book->CancelAsyncReflowOpen();
  }

  CloseBook();
  nav_.mode = AppMode::Quit;

#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "SHUTDOWN cancel workers done removed_jobs=%u current_session=%u opening_session=%u",
           (unsigned)removed_jobs, reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
  DBG_LOGF(this, "SHUTDOWN end mode=%d", (int)nav_.mode);
#endif
}

void App::AptHookCallback(APT_HookType hook, void *param)
{
  App *app = static_cast<App *>(param);
  if (app)
    app->HandleAppletHook(hook);
}

void App::HandleAppletHook(APT_HookType hook)
{
  // Do NOT log here. This callback runs on the APT system thread, not the main
  // thread. Calling DBG_LOGF would access nav_/reader_state_ without
  // synchronization and would invoke PrintStatus, which does LightLock + fflush
  // (SD card I/O) from the hook thread — the same class of bug documented in
  // the APTHOOK_ONEXIT comment below. Lifecycle events are logged in
  // HandleAppletSuspend/HandleAppletResume on the main thread instead.
  switch (hook)
  {
  case APTHOOK_ONSUSPEND:
    // Signal suspend state to the main thread. All browser/reader mutations
    // are deferred to HandleAppletSuspend() on the main thread to avoid
    // cross-thread writes to nav_ and reader state, and to avoid dereferencing
    // Book pointers from the APT hook thread.
    lifecycle_state_.SetSuspended(true);
    lifecycle_state_.SetResumePending(false);
    lifecycle_state_.SetSuspendHandled(false);
    break;
  case APTHOOK_ONRESTORE:
  case APTHOOK_ONWAKEUP:
    lifecycle_state_.SetSuspended(false);
    lifecycle_state_.SetResumePending(true);
    break;
  case APTHOOK_ONEXIT:
    // Only set the quit flag here. PersistPrefs() writes to the SD card and
    // must NOT run inside an APT hook callback — doing so blocks the HOME Menu
    // from receiving the acknowledgment within its expected timing window,
    // which can cause the HOME Menu process to crash. Prefs are saved in
    // PrepareForShutdown() after aptMainLoop() returns.
    lifecycle_state_.SetExitRequested(true);
    break;
  default:
    break;
  }
}

void App::HandleAppletSuspend()
{
  if (lifecycle_state_.IsSuspendHandled())
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "APPLET suspend begin mode=%d current_session=%u opening_session=%u",
           (int)nav_.mode, reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
  lifecycle_state_.SetSuspendHandled(true);
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  ResetPageRepeat();
  const size_t removed_jobs = PauseBrowserJobs();
#ifndef DSLIBRIS_DEBUG
  (void)removed_jobs;
#endif
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "APPLET suspend workers paused removed_jobs=%u",
           (unsigned)removed_jobs);
#endif
  OnReaderAppletSuspended();
  // Free FreeType glyph bitmap cache to release RAM for the HOME menu.
  // Bounded at 512 glyphs per face × multiple faces × per-glyph buffer alloc
  // — can be hundreds of KB. Cache re-warms transparently on resume.
  if (ts)
    ts->ClearCache();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "APPLET suspend end mode=%d current_session=%u opening_session=%u",
           (int)nav_.mode,
           reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
}

void App::HandleAppletResume()
{
  if (!lifecycle_state_.IsResumePending())
    return;
  lifecycle_state_.SetResumePending(false);
  lifecycle_state_.SetSuspendHandled(false);
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms = osGetTime();
  nav_.browser.view_dirty = true;
  ResetPageRepeat();
  nav_.prefs.view_dirty = true;
  if (ts)
    ts->MarkAllScreensDirty();
  RequestStatusRedraw();
  OnReaderAppletResumed();
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "APPLET resumed mode=%d current_session=%u opening_session=%u",
           (int)nav_.mode, reader_state_.current_book_session_id,
           reader_state_.opening.session_id);
#endif
}
