#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
typedef uint64_t u64;
typedef uint16_t u16;
typedef int Result;
#define R_SUCCEEDED(result) ((result) >= 0)
struct LightEvent {};
struct fz_context {};
struct FakeThread { int timeouts=2; bool exited=false, freed=false; };
static std::vector<std::string> events;
static int clears=0, destroyed=0;
void LightEvent_Clear(LightEvent *) { ++clears; }
void LightEvent_Signal(LightEvent *) { events.push_back("signal"); }
struct Book {
  struct MuPdfState {
    struct MuPdfWorker {
      bool job_submitted=true, job_pending=true, shutdown_requested=false;
      int job_strip_y0=2, job_strip_y1=10;
      LightEvent done_event, submit_event;
      FakeThread *thread_handle=nullptr;
      fz_context *worker_ctx=nullptr;
      ~MuPdfWorker() {
        assert(!thread_handle && !worker_ctx);
        ++destroyed;
      }
    };
    MuPdfWorker *worker=nullptr;
    bool worker_init_attempted=true;
    struct {
      bool active=true;
      int strips_completed=1, partial_width=10, partial_height=10;
      std::vector<u16> partial_pixels;
    } incremental;
  };
};
static Book::MuPdfState *running=nullptr;
Result threadJoin(FakeThread *t, u64 timeout) {
  assert(timeout > 0);
  assert(running && running->worker->shutdown_requested);
  assert(running->incremental.active && running->incremental.partial_pixels.size() == 100);
  assert(running->worker->job_strip_y0 == 2 && running->worker->job_strip_y1 == 10);
  events.push_back("join");
  if (t->timeouts-- > 0) return -1;
  t->exited=true; return 0;
}
void threadFree(FakeThread *t) {
  assert(t->exited && !t->freed); t->freed=true; events.push_back("free");
}
void fz_drop_context(fz_context *) {
  assert(!running || running->worker->thread_handle == nullptr);
  events.push_back("drop");
}
void ShutdownMuPdfWorker(Book::MuPdfState *);
#include "mupdf_cancel_under_test.inc"
static void Seed(Book::MuPdfState &s) { s.incremental.partial_pixels.assign(100, 42); }
static void AssertCleared(const Book::MuPdfState &s) {
  assert(!s.incremental.active && s.incremental.partial_pixels.empty());
  assert(s.incremental.strips_completed == 0);
  assert(s.incremental.partial_width == 0 && s.incremental.partial_height == 0);
}
int main() {
  Book::MuPdfState s; FakeThread thread; fz_context context;
  s.worker=new Book::MuPdfState::MuPdfWorker;
  s.worker->thread_handle=&thread; s.worker->worker_ctx=&context;
  Seed(s); running=&s;
  CancelMuPdfIncrementalRenderState(&s);
  assert((events == std::vector<std::string>{"signal", "join", "signal", "join", "signal", "join", "free", "drop"}));
  assert(thread.freed && destroyed == 1 && !s.worker && !s.worker_init_attempted);
  AssertCleared(s); running=nullptr;

  // A completed submitted job is acknowledged without destroying the worker.
  Book::MuPdfState completed; Seed(completed);
  completed.worker=new Book::MuPdfState::MuPdfWorker;
  completed.worker->job_pending=false;
  events.clear(); CancelMuPdfIncrementalRenderState(&completed);
  assert(events.empty() && clears == 1 && destroyed == 1);
  assert(!completed.worker->job_submitted && completed.worker->job_strip_y0 == 0 && completed.worker->job_strip_y1 == 0);
  assert(completed.worker_init_attempted); AssertCleared(completed);
  CancelMuPdfIncrementalRenderState(&completed);
  assert(clears == 1); // Acknowledgement is idempotent.
  ShutdownMuPdfWorker(&completed);

  // Failed/partial initialization has no thread to join, but owns a context.
  Book::MuPdfState partial; Seed(partial);
  partial.worker=new Book::MuPdfState::MuPdfWorker;
  partial.worker->worker_ctx=&context;
  events.clear(); CancelMuPdfIncrementalRenderState(&partial);
  assert((events == std::vector<std::string>{"signal", "drop"}));
  assert(!partial.worker && !partial.worker_init_attempted); AssertCleared(partial);
  Book::MuPdfState missing; Seed(missing);
  events.clear(); CancelMuPdfIncrementalRenderState(&missing);
  assert(events.empty() && missing.worker_init_attempted); AssertCleared(missing);
  CancelMuPdfIncrementalRenderState(nullptr); ShutdownMuPdfWorker(nullptr);
  puts("PASS: cancellation retries the actual join before disposing worker and strip state");
}
