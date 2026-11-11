#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

typedef uint64_t u64;
static u64 osGetTime() {
  return 1000;
}
#define DBG_LOGF(...) ((void)0)
enum class AppMode { Opening, Browser };
struct FrameInput {
  unsigned keys_down = 0;
  u64 timestamp_ms = 1000;
};
struct Book {
  bool HasPendingReflowNavigation() { std::abort(); }
  int AdvanceReflowPageCache() { std::abort(); }
  void SetLayoutRevision(unsigned) {
    std::abort();
  }
  unsigned GetPageCount() {
    std::abort();
  }
  void SetPosition(int) {
    std::abort();
  }
};
static unsigned steps = 0, cancellations = 0, destructions = 0;
struct ReflowPreparation {
  enum Result { Pending, Complete, Cancelled, Failed, NoMemory, IoError, Paused };
  Result result = Pending, outcome = NoMemory;
  ~ReflowPreparation() {
    ++destructions;
  }
  Result GetResult() const {
    return result;
  }
  Result Step() {
    ++steps;
    return result = outcome;
  }
  void Cancel() {
    ++cancellations;
  }
  const char *Label() const {
    return "test preparation failure";
  }
  unsigned Progress() const {
    return 0;
  }
  unsigned Total() const {
    return 0;
  }
  void NoteFrame(unsigned long long, unsigned) {}
  void NoteSplash(unsigned) {}
  unsigned FrameBudgetMs() const {
    return 30;
  }
  bool PersistenceFailed() const {
    std::abort();
  }
};
struct App {
  struct {
    unsigned b = 1, start = 2, a = 4;
  } key;
  AppMode mode = AppMode::Opening;
  Book *current = nullptr;
  Book *GetCurrentBook() { return current; }
  bool abort_work = false, browser_dirty = false;
  unsigned resets = 0;
  std::vector<std::string> displays;
  bool ShouldAbortWork() {
    return abort_work;
  }
  void SetCurrentBook(Book *book) {
    assert(!book);
    current = book;
  }
  void SetCurrentBookSessionId(unsigned session) {
    assert(!session);
  }
  void SetMode(AppMode next) {
    mode = next;
  }
  void SetBrowserDirty(bool dirty) {
    browser_dirty = dirty;
  }
  unsigned GetLayoutRevision() {
    std::abort();
  }
  bool IsOpeningNeedsRelayout() {
    std::abort();
  }
  unsigned GetOpeningOldPageCount() {
    std::abort();
  }
  unsigned GetOpeningOldPosition() {
    std::abort();
  }
  unsigned GetOpeningSessionId() {
    std::abort();
  }
  std::vector<unsigned> &MutableOpeningOldBookmarks() {
    std::abort();
  }
};
namespace deferred_relayout_utils {
struct Plan {
  bool has_remap;
  int mapped_position;
  std::vector<unsigned> mapped_bookmarks;
};
static Plan BuildOpenRelayoutPlan(bool, bool, unsigned, unsigned, unsigned,
                                  const std::vector<unsigned> &) {
  std::abort();
}
} // namespace deferred_relayout_utils
static void ApplyRemappedBookmarks(Book *, const std::vector<unsigned> &) {
  std::abort();
}
static void ClearSavedRelayoutState(const Book *) { std::abort(); }
static void ResetOpeningState(App *app) {
  ++app->resets;
}
static void DrawOpeningSplashImpl(App *app, unsigned, unsigned,
                                  const char *label) {
  app->displays.push_back(label);
}
struct ReaderController {
  App &app_;
  std::unique_ptr<ReflowPreparation> preparation_{
      new ReflowPreparation};
  Book *cache_preparing_;
  std::string cache_label_;
  u64 cache_draw_ms_ = 0;
  ReaderController(App &app, Book &book) : app_(app), cache_preparing_(&book) {}
  bool AdvanceCachePreparation(const FrameInput &);
  void OpenBook() {
    std::abort();
  }
};
#include "preparation_error_owner.inc"

int main() {
  for (auto failure :
       {ReflowPreparation::NoMemory, ReflowPreparation::IoError,
        ReflowPreparation::Failed}) {
    for (unsigned back : {1u, 2u}) {
      steps = cancellations = destructions = 0;
      App app;
      Book book;
      ReaderController reader(app, book);
      app.current = &book;
      reader.preparation_->outcome = failure;
      assert(reader.AdvanceCachePreparation({}));
      assert(app.mode == AppMode::Opening && !app.browser_dirty && !app.resets);
      assert(cancellations == 1 && destructions == 1);
      assert(!app.current);
      assert(app.displays.back().find("test preparation failure") !=
             std::string::npos);
      assert(app.displays.back().find("B:") != std::string::npos);
      FrameInput idle;
      idle.timestamp_ms = 2000;
      assert(reader.AdvanceCachePreparation(idle));
      assert(steps == 1 && app.mode == AppMode::Opening);
      app.abort_work = true;
      assert(reader.AdvanceCachePreparation(idle));
      app.abort_work = false;
      idle.keys_down = back;
      assert(reader.AdvanceCachePreparation(idle));
      assert(app.mode == AppMode::Browser && app.browser_dirty &&
             app.resets == 1);
      assert(!reader.cache_preparing_ && !reader.preparation_);
    }
  }
  puts("PASS: opening errors release resources, stay visible and wait for "
       "B/START");
}
