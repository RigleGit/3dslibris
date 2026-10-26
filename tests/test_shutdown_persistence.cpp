#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
typedef uint64_t u64;
static u64 osGetTime() { return 1000; }
static const u64 kProgressAutosaveIntervalMs = 30000;
#define DBG_LOG(...) ((void)0)
#define DBG_LOGF(...) ((void)0)
enum class AppMode { Book, Browser, Quit = 7 };
struct Book {
  int position = 38, close_calls = 0;
  void CancelPendingEpubPageCacheSave() {}
  void RequestAbortOpen() {}
  void CancelAsyncReflowOpen() {}
  void Close() { ++close_calls; }
};
namespace book_renderer { void CancelFixedLayoutDeferredWork(Book *) {} }
struct App;
struct Prefs {
  App *app;
  explicit Prefs(App *owner) : app(owner) {}
  int attempts = 0, failures_remaining = 0;
  bool write_pending = false;
  u64 write_due_ms = 0;
  std::vector<int> saved_positions;
  int Write();
  bool FlushPendingWrite(bool force = false);
};
class ReaderController;
struct App {
  struct Lifecycle {
    bool prepared = false;
    bool IsShutdownPrepared() const { return prepared; }
    void MarkShutdownPrepared() { prepared = true; }
    void SetResumePending(bool) {}
    void SetSuspendHandled(bool) {}
    void SetExitRequested(bool) {}
  } lifecycle_state_;
  struct { struct { bool wait_input_release; u64 last_interaction_ms; } browser;
           AppMode mode = AppMode::Book; } nav_;
  struct { struct { Book *book = nullptr; } opening; } reader_state_;
  bool pending_boot_reopen_ = false, skip_next_browser_present_ = false;
  Book *current = nullptr;
  Prefs storage{this};
  Prefs *prefs = &storage;
  ReaderController *reader_controller_ = nullptr;
  Book *GetCurrentBook() { return current; }
  void SetCurrentBook(Book *book) { current = book; }
  void PersistPrefs() { prefs->Write(); }
  void ResetPageRepeat() {}
  size_t PauseBrowserJobs() { return 0; }
  void SetInlineLinkFocusActive(bool) {}
  void SetInlineLinkHoldArmed(bool) {}
  void SetInlineLinkHoldConsumed(bool) {}
  void SetInlineLinkHoldStartedAtMs(u64) {}
  void SetCurrentBookSessionId(unsigned) {}
  void SetPdfDeferredReadyAtMs(u64) {}
  void CloseBook();
  void PrepareForShutdown();
};
int Prefs::Write() {
  ++attempts;
  if (failures_remaining > 0) {
    --failures_remaining; write_pending = true; return 255;
  }
  saved_positions.push_back(app->current ? app->current->position : -1);
  write_pending = false;
  return 0;
}
class ReaderController {
public:
  App &app_;
  Book *progress_autosave_book_ = nullptr;
  bool progress_autosave_dirty_ = false;
  u64 last_progress_persist_ms_ = 0;
  explicit ReaderController(App &app) : app_(app) { app.reader_controller_ = this; }
  void TryPersistProgress(Book *, bool);
  void ResetProgressAutosave(Book *);
  void CloseBook(bool persist_progress = true);
  void InvalidateImageViewer() {}
  void InvalidateSearch(bool) {}
  void ClearDeferredRelayoutState() {}
};
void App::CloseBook() { reader_controller_->CloseBook(); }
static void ResetOpeningState(App *) {}
#include "shutdown_owner.inc"

int main() {
  for (bool with_book : {true, false}) {
    App app; ReaderController reader(app); Book book;
    if (with_book) app.current = &book;
    app.PrepareForShutdown();
    assert(app.storage.attempts == 1);
    assert(app.storage.saved_positions == std::vector<int>{with_book ? 38 : -1});
    assert(book.close_calls == (with_book ? 1 : 0));
    assert(app.current == nullptr && app.nav_.mode == AppMode::Quit);
    app.PrepareForShutdown();
    assert(app.storage.attempts == 1);
  }
  // An explicit normal close still saves the latest page before clearing it.
  App normal; ReaderController reader(normal); Book book; normal.current = &book;
  normal.CloseBook();
  assert(normal.storage.saved_positions == std::vector<int>{38});
  // A failed final write is retried once while the book is still available.
  App retry; ReaderController retry_reader(retry); Book retry_book;
  retry.current = &retry_book; retry.storage.failures_remaining = 1;
  retry.PrepareForShutdown();
  assert(retry.storage.attempts == 2 && !retry.storage.write_pending);
  assert(retry.storage.saved_positions == std::vector<int>{38});
  puts("PASS: one final preference write, progress before close and bounded failure retry");
}
