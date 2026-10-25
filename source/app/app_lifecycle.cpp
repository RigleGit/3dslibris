/*
    3dslibris - app_lifecycle.cpp
    Extracted from app.cpp. Holds the APT applet lifecycle path
    (PrepareForShutdown, AptHookCallback, HandleAppletHook,
    HandleAppletSuspend, HandleAppletResume).

    Suspend hooks only signal workers. Joins, logging and resource cleanup
    are deferred until control returns to the main loop.
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
  // Do not finish a large optional cache after aptMainLoop has requested exit.
  // This is normal main-thread cleanup, never the APT hook.
  if (Book *current = GetCurrentBook())
    current->CancelPendingEpubPageCacheSave();
  if (reader_state_.opening.book && reader_state_.opening.book != GetCurrentBook())
    reader_state_.opening.book->CancelPendingEpubPageCacheSave();
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
    opening_book->CancelPendingEpubPageCacheSave();
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
  // libctru invokes suspend/sleep hooks synchronously from aptMainLoop.
  // Its body will not run again until HOME/sleep returns. Prepare here, but
  // keep this path free of SD writes, rendering, joins and book cache destruction.
  // The reader may release its isolated CPU-only image-viewer buffers here.
  switch (hook)
  {
  case APTHOOK_ONSUSPEND:
  case APTHOOK_ONSLEEP:
    lifecycle_state_.SetSuspended(true);
    lifecycle_state_.SetResumePending(false);
    OnReaderAppletSuspendRequested();
    lifecycle_state_.SetSuspendHandled(true);
    break;
  case APTHOOK_ONRESTORE:
  case APTHOOK_ONWAKEUP:
    lifecycle_state_.SetSuspended(false);
    lifecycle_state_.SetResumePending(true);
    break;
  case APTHOOK_ONEXIT:
    // Persist preferences in PrepareForShutdown after aptMainLoop returns.
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
  OnReaderAppletSuspendRequested();
  lifecycle_state_.SetSuspendHandled(true);
}

void App::HandleAppletResume()
{
  if (!lifecycle_state_.IsResumePending())
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "APPLET resume prepared_before_wait=%u",
           lifecycle_state_.IsSuspendHandled() ? 1u : 0u);
#endif
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
