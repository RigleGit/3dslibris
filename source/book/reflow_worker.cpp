#include "book/book.h"

#include <3ds.h>

#include "shared/debug_log.h"
#include "book/book_parser.h"
#include "reader/reflow_open_gate_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/open_cancel_poll.h"
#include "ui/text.h"

namespace {

bool DetectNew3dsForReflow() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

} // namespace

struct Book::ReflowWorkerState {
  struct Worker {
    Book *owner;
    volatile bool shutdown_requested;
    volatile bool job_pending;
    bool job_submitted;
    u8 job_result;
    u64 submitted_at_ms;
    u64 started_at_ms;
    u64 finished_at_ms;
    unsigned int session_id;
    LightEvent submit_event;
    LightEvent done_event;
    Thread thread_handle;

    Worker()
        : owner(NULL), shutdown_requested(false), job_pending(false),
          job_submitted(false), job_result(1), submitted_at_ms(0),
          started_at_ms(0), finished_at_ms(0), session_id(0),
          thread_handle(NULL) {}
  };

  bool is_new_3ds;
  bool open_pending;
  bool open_completed;
  u8 open_result;
  Worker *worker;
  bool worker_init_attempted;

  ReflowWorkerState()
      : is_new_3ds(false), open_pending(false), open_completed(false),
        open_result(1), worker(NULL), worker_init_attempted(false) {}
};

namespace {

static const size_t kReflowWorkerStackBytes = 256u * 1024u;

void ReflowWorkerThreadFunc(void *arg) {
  Book::ReflowWorkerState::Worker *w =
      static_cast<Book::ReflowWorkerState::Worker *>(arg);
  if (!w || !w->owner)
    return;
  Book *book = w->owner;

  while (true) {
    LightEvent_Wait(&w->submit_event);
    LightEvent_Clear(&w->submit_event);

    if (__atomic_load_n(&w->shutdown_requested, __ATOMIC_ACQUIRE))
      break;
    if (!__atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
      continue;

    w->started_at_ms = osGetTime();
    if (book->GetStatusReporter()) {
      const u64 queue_ms =
          (w->submitted_at_ms && w->started_at_ms >= w->submitted_at_ms)
              ? (w->started_at_ms - w->submitted_at_ms)
              : 0;
      DBG_LOGF(book->GetStatusReporter(),
               "REFLOW[w]: open begin session=%u queue_ms=%llu book=%s",
               (unsigned)w->session_id, (unsigned long long)queue_ms,
               book->GetFileName() ? book->GetFileName() : "");
    }
    w->job_result = book_parser::OpenPrepared(book);
    w->finished_at_ms = osGetTime();
    if (book->GetStatusReporter()) {
      const u64 worker_ms = (w->finished_at_ms >= w->started_at_ms)
                                ? (w->finished_at_ms - w->started_at_ms)
                                : 0;
      DBG_LOGF(book->GetStatusReporter(),
               "REFLOW[w]: open finish session=%u rc=%u worker_ms=%llu book=%s",
               (unsigned)w->session_id, (unsigned)w->job_result,
               (unsigned long long)worker_ms,
               book->GetFileName() ? book->GetFileName() : "");
    }
    __atomic_store_n(&w->job_pending, false, __ATOMIC_RELEASE);
    LightEvent_Signal(&w->done_event);
  }
}

} // namespace

void Book::PrepareForOpen() {
  Text *text = GetText();
  if (text)
    text->SetStyle(TEXT_STYLE_REGULAR);
  ResetReadingPaceEstimate();
  ClearOpenAbortRequest();
  open_cancel_poll::Reset();
  tocResolveTried = false;
  tocResolved = false;
  ClearTocConfidence();
  ClearChapterAnchors();
}

bool Book::SupportsAsyncReflowOpen() const {
  return reader::ShouldUseAsyncReflowOpen(UsesTextLayoutSettings());
}

bool Book::StartAsyncReflowOpen(unsigned int session_id) {
  if (debug_runtime::BackgroundWorkersDisabled())
    return false;
  if (!SupportsAsyncReflowOpen())
    return false;

  if (!reflow_worker_state) {
    reflow_worker_state = new ReflowWorkerState();
    reflow_worker_state->is_new_3ds = DetectNew3dsForReflow();
  }
  if (!reflow_worker_state || !reflow_worker_state->is_new_3ds)
    return false;

  if (!reflow_worker_state->worker &&
      !reflow_worker_state->worker_init_attempted) {
    reflow_worker_state->worker_init_attempted = true;
    ReflowWorkerState::Worker *w = new ReflowWorkerState::Worker();
    w->owner = this;
    LightEvent_Init(&w->submit_event, RESET_STICKY);
    LightEvent_Init(&w->done_event, RESET_STICKY);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    reflow_worker_state->worker = w;
    // Debug builds push parser, XML, and TOC work through this worker.
    // Small stacks here can corrupt unrelated libctru globals and then crash
    // the next main-thread HID poll instead of failing at the real site.
    w->thread_handle = threadCreate(
        ReflowWorkerThreadFunc, w, kReflowWorkerStackBytes, prio + 1, 1, false);
    if (!w->thread_handle) {
      delete w;
      reflow_worker_state->worker = NULL;
      return false;
    }
    if (GetStatusReporter())
      DBG_LOG(GetStatusReporter(),
              "REFLOW[w]: worker thread started on core 1");
  }
  if (!reflow_worker_state->worker)
    return false;

  // If a previous worker was abandoned during applet suspension, clean it up
  // now (non-blocking) before starting a new open.
  Book::ReflowWorkerState::Worker *old_w = reflow_worker_state->worker;
  if (old_w->thread_handle &&
      __atomic_load_n(&old_w->shutdown_requested, __ATOMIC_ACQUIRE)) {
    Result join_rc = threadJoin(old_w->thread_handle, 0);
    if (R_SUCCEEDED(join_rc)) {
      threadFree(old_w->thread_handle);
      delete old_w;
      reflow_worker_state->worker = NULL;
      reflow_worker_state->worker_init_attempted = false;
      return false;
    }
    // Worker is still finishing its last parse after a previous cancel;
    // can't reuse it yet. Fall back to synchronous open.
    return false;
  }

  Book::ReflowWorkerState::Worker *w = reflow_worker_state->worker;
  if (w->job_submitted || __atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
    return false;

  PrepareForOpen();
  SetOpenSessionId(session_id);
  reflow_worker_state->open_pending = true;
  reflow_worker_state->open_completed = false;
  reflow_worker_state->open_result = 1;
  w->job_result = 1;
  w->submitted_at_ms = osGetTime();
  w->started_at_ms = 0;
  w->finished_at_ms = 0;
  w->session_id = session_id;
  LightEvent_Clear(&w->done_event);
  __atomic_store_n(&w->job_pending, true, __ATOMIC_RELEASE);
  w->job_submitted = true;
  LightEvent_Signal(&w->submit_event);
  return true;
}

bool Book::PumpAsyncReflowOpen() {
  if (!reflow_worker_state || !reflow_worker_state->open_pending ||
      !reflow_worker_state->worker) {
    return false;
  }

  Book::ReflowWorkerState::Worker *w = reflow_worker_state->worker;
  if (!w->job_submitted || __atomic_load_n(&w->job_pending, __ATOMIC_ACQUIRE))
    return false;

  LightEvent_Wait(&w->done_event);
  LightEvent_Clear(&w->done_event);
  w->job_submitted = false;
  reflow_worker_state->open_pending = false;
  reflow_worker_state->open_completed = true;
  reflow_worker_state->open_result = w->job_result;
  return true;
}

bool Book::IsAsyncReflowOpenPending() const {
  return reflow_worker_state && reflow_worker_state->open_pending;
}

u8 Book::ConsumeAsyncReflowOpenResult() {
  if (!reflow_worker_state || !reflow_worker_state->open_completed)
    return 1;
  reflow_worker_state->open_completed = false;
  return reflow_worker_state->open_result;
}

void Book::CancelAsyncReflowOpen() {
  if (!reflow_worker_state)
    return;

  Book::ReflowWorkerState::Worker *w = reflow_worker_state->worker;
  reflow_worker_state->open_pending = false;
  reflow_worker_state->open_completed = false;
  reflow_worker_state->open_result = 1;

  if (!w)
    return;

  __atomic_store_n(&w->shutdown_requested, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&w->submit_event);
  if (w->thread_handle) {
#ifdef DSLIBRIS_DEBUG
    if (GetStatusReporter())
      DBG_LOGF(GetStatusReporter(),
               "REFLOW cancel: joining thread session=%u book=%s",
               (unsigned)w->session_id, GetFileName() ? GetFileName() : "");
#endif
    // All current callers either tear down or immediately reuse this Book.
    // Returning while the worker still runs can corrupt the Book state when
    // Close() frees pages/caches or when a new open starts on the same object.
    // Async opens are kept alive during HOME suspend, so explicit cancellation
    // is allowed to wait here until the worker exits cooperatively.
    while (w->thread_handle) {
      Result join_rc = threadJoin(w->thread_handle, 100 * 1000000ULL);
      if (R_SUCCEEDED(join_rc)) {
        threadFree(w->thread_handle);
        w->thread_handle = NULL;
        break;
      }
    }
  }
  if (!w->thread_handle) {
    delete w;
    reflow_worker_state->worker = NULL;
    reflow_worker_state->worker_init_attempted = false;
  }
}

void Book::SignalReflowWorkerShutdown() {
  // Non-blocking shutdown for the APT suspend path. Marks the worker for
  // exit and wakes it from LightEvent_Wait, but does NOT join. Joining can
  // block 100ms+ if a parse is in flight, and any blocking inside the
  // suspend handler risks exceeding the HOME menu's acknowledgment window
  // (the same class of bug as the previous SD-card-I/O-in-APT-hook crash).
  // FinishShutdownReflowWorker() on resume — or the existing non-blocking
  // join branch in StartAsyncReflowOpen — completes cleanup.
  if (!reflow_worker_state)
    return;
  Book::ReflowWorkerState::Worker *w = reflow_worker_state->worker;
  reflow_worker_state->open_pending = false;
  reflow_worker_state->open_completed = false;
  reflow_worker_state->open_result = 1;
  if (!w)
    return;
  __atomic_store_n(&w->shutdown_requested, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&w->submit_event);
}

void Book::FinishShutdownReflowWorker() {
  // Completes the deferred cleanup from SignalReflowWorkerShutdown(). Called
  // on resume from the main thread. Non-blocking: if the worker still hasn't
  // exited (e.g. mid-parse strip), bail out and let the next
  // StartAsyncReflowOpen call try again via its existing non-blocking branch.
  if (!reflow_worker_state)
    return;
  Book::ReflowWorkerState::Worker *w = reflow_worker_state->worker;
  if (!w)
    return;
  if (!__atomic_load_n(&w->shutdown_requested, __ATOMIC_ACQUIRE))
    return;
  if (w->thread_handle) {
    Result join_rc = threadJoin(w->thread_handle, 0);
    if (R_FAILED(join_rc))
      return;
    threadFree(w->thread_handle);
    w->thread_handle = NULL;
  }
  delete w;
  reflow_worker_state->worker = NULL;
  reflow_worker_state->worker_init_attempted = false;
}

void Book::ResetReflowWorkerState() {
  if (!reflow_worker_state)
    return;
  delete reflow_worker_state;
  reflow_worker_state = NULL;
}
