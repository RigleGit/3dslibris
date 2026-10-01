#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#define DBG_LOGF(...) ((void)0)
enum APT_HookType { APTHOOK_ONSUSPEND, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP,
                    APTHOOK_ONWAKEUP, APTHOOK_ONEXIT };
struct State {
  bool suspended=false, resume=false, handled=false, exit=false;
  void SetSuspended(bool v) { suspended=v; }
  void SetResumePending(bool v) { resume=v; }
  void SetSuspendHandled(bool v) { handled=v; }
  void SetExitRequested(bool v) { exit=v; }
  bool IsSuspended() const { return suspended; }
};
static std::vector<std::string> events;
struct Book;
struct App {
  State lifecycle_state_;
  Book *current=nullptr;
  bool touch=true;
  int last_x=10, last_y=20, deferred=30;
  Book *GetCurrentBook() { return current; }
  void SetPdfTouchDragActive(bool v) { touch=v; }
  void SetPdfTouchLastX(int v) { last_x=v; }
  void SetPdfTouchLastY(int v) { last_y=v; }
  void SetPdfDeferredReadyAtMs(int v) { deferred=v; }
  void OnReaderAppletSuspendRequested();
  void HandleAppletHook(APT_HookType);
};
struct Book {
  struct MuPdfState { void *worker=(void*)1; } pdf;
  struct CbzState {} cbz;
  MuPdfState *mupdf_state=&pdf;
  CbzState *cbz_state=&cbz;
  enum Format { Reflowable, Pdf, Cbz } format=Pdf;
  bool IsPdf() { return format == Pdf; }
  bool IsCbz() { return format == Cbz; }
  void SuspendFixedLayoutWorkers();
  void ResumeFixedLayoutWorkers();
  void ResetCbzTransientViewState(bool restart) {
    assert(restart); events.push_back("cbz-restart");
  }
  void ReleaseMuPdfMemoryForSuspend() {
    assert(!pdf.worker && "resources must survive until worker join");
    events.push_back("pdf-release");
  }
};
void SignalMuPdfWorkerShutdown(Book::MuPdfState *) { events.push_back("pdf-signal"); }
void SignalCbzWorkerShutdown(Book::CbzState *) { events.push_back("cbz-signal"); }
void ShutdownMuPdfWorker(Book::MuPdfState *s) {
  events.push_back("pdf-join"); s->worker=nullptr;
}
void InitMuPdfWorker(Book::MuPdfState *s) {
  assert(events.back() == "pdf-release");
  events.push_back("pdf-init"); s->worker=(void*)1;
}
struct ReaderController {
  App &app_;
  explicit ReaderController(App &app) : app_(app) {}
  void OnAppletSuspendRequested();
};
void App::OnReaderAppletSuspendRequested() {
  assert(lifecycle_state_.suspended && !lifecycle_state_.resume);
  ReaderController(*this).OnAppletSuspendRequested();
}
#include "home_suspend_under_test.inc"
int main() {
  App app; Book b; app.current=&b;
  app.lifecycle_state_.resume=true;
  // The complete synchronous hook path must prepare the current book before
  // aptMainLoop yields; only the main-loop resume path may join or free.
  app.HandleAppletHook(APTHOOK_ONSUSPEND);
  assert((events == std::vector<std::string>{"pdf-signal"}));
  assert(app.lifecycle_state_.suspended && app.lifecycle_state_.handled);
  assert(!app.touch && app.last_x == -1 && app.last_y == -1 && app.deferred == 0);
  app.HandleAppletHook(APTHOOK_ONRESTORE);
  assert(!app.lifecycle_state_.suspended && app.lifecycle_state_.resume);
  b.ResumeFixedLayoutWorkers();
  assert((events == std::vector<std::string>{"pdf-signal", "pdf-join", "pdf-release", "pdf-init"}));

  events.clear(); b.format=Book::Cbz;
  app.HandleAppletHook(APTHOOK_ONSLEEP);
  assert((events == std::vector<std::string>{"cbz-signal"}));
  app.HandleAppletHook(APTHOOK_ONWAKEUP);
  assert(!app.lifecycle_state_.suspended && app.lifecycle_state_.resume);
  b.ResumeFixedLayoutWorkers();
  assert((events == std::vector<std::string>{"cbz-signal", "cbz-restart"}));
  events.clear();
  b.format=Book::Reflowable;
  app.HandleAppletHook(APTHOOK_ONSUSPEND);
  assert(events.empty());
  b.format=Book::Pdf; b.pdf.worker=nullptr;
  app.HandleAppletHook(APTHOOK_ONSUSPEND);
  assert(events.empty());
  b.ResumeFixedLayoutWorkers();
  assert((events == std::vector<std::string>{"pdf-release", "pdf-init"}));
  events.clear(); b.mupdf_state=nullptr; b.cbz_state=nullptr;
  b.SuspendFixedLayoutWorkers(); b.ResumeFixedLayoutWorkers();
  app.current=nullptr;
  app.HandleAppletHook(APTHOOK_ONSUSPEND);
  assert(events.empty());
  app.HandleAppletHook(APTHOOK_ONEXIT);
  assert(app.lifecycle_state_.exit && events.empty());
  puts("PASS: HOME hooks signal only; resume joins before releasing and restarting");
}
