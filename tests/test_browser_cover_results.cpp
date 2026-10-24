// Exercise the actual embedded-cover job owner for five backend outcomes.
// Folder I/O/cache/decoder integration lives in test_folder_cover_jobs.
#define osGetTime UnusedPlatformTime
#include <3ds.h>
#undef osGetTime
#include "app/app_mode.h"
#include "app/library_controller.h"
#include "book/book_context.h"
#include "book/book_parser.h"
#include "book/cover_layout_constants.h"
#include "formats/common/book_error.h"
#include "formats/common/file_read_utils.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_job_queue_utils.h"
#include "library/browser_view_utils.h"
#include "library/browser_warmup_utils.h"
#include "library/cover_cache.h"
#include "library/cover_override_utils.h"
#include "shared/app_flow_utils.h"
#include "shared/aspect_fit_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/fixed_layout_perf.h"
#include "shared/home_button_guard.h"
#include "shared/status_reporter.h"
#include "shared/string_utils.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

struct Book {
  bool browser_folder_entry = false, metadataIndexTried = false;
  bool tocResolveTried = false, tocResolved = false;
  std::string browser_folder_path, browser_folder_display_name;
  std::string browser_folder_cover_path, foldername, filename, title;
  std::string author, series, language, publisher, published, subjects, description;
  std::string coverImagePath;
  format_t format = FORMAT_UNDEF;
  u16 *coverPixels = nullptr;
  int coverWidth = 0, coverHeight = 0;
  uint8_t coverAttempts = 0;
  u64 coverRetryAfterMs = 0;
  explicit Book(const BookContext &) {}
  ~Book() { delete[] coverPixels; }
  bool IsBrowserFolder() const { return browser_folder_entry; }
  const char *GetFolderName() const { return foldername.c_str(); }
  const char *GetFileName() const { return filename.c_str(); }
  const std::string &GetBrowserFolderCoverPath() const { return browser_folder_cover_path; }
  void ClearBrowserDisplayNameCache() {}
  void SetBrowserFolderEntry(const std::string &, const std::string &, const std::string &);
};
struct Text { int screenright = 0; void MarkScreenDirty(int) {} };
struct Prefs { BrowserViewMode browser_view_mode = BROWSER_VIEW_GALLERY; };
struct ChapterMenu { void Init() {} void SetDirty(bool) {} };
struct App : IStatusReporter {
  std::unique_ptr<Text> ts{new Text};
  std::unique_ptr<Prefs> prefs{new Prefs};
  std::vector<Book *> books;
  unsigned char paraspacing = 0, paraindent = 0, orientation = 0;
  bool publisher_text_indent = false, publisher_block_margins = false;
  bool publisher_horizontal_margins = false;
  bool suspended = false, abort_work = false, input_release = false, dirty = false;
  bool new_3ds = false;
  int page_start = 0;
  Book *selected = nullptr;
  AppMode mode = AppMode::Browser;
  u64 last_interaction = 0;
  ChapterMenu *chaptermenu = nullptr;
  ~App() { for (Book *book : books) delete book; }
  void PrintStatus(const char *) override {}
  void PrintStatus(std::string) override {}
  bool ShouldAbortWork() const override { return abort_work; }
  bool IsAppletSuspended() const { return suspended; }
  bool IsNew3dsDevice() const { return new_3ds; }
  bool IsBrowserWaitingInputRelease() const { return input_release; }
  u64 GetBrowserLastInteractionMs() const { return last_interaction; }
  AppMode GetMode() const { return mode; }
  Book *GetSelectedBook() const { return selected; }
  Book *GetCurrentBook() const { return nullptr; }
  int BookCount() const { return (int)books.size(); }
  int GetBrowserPageStart() const { return page_start; }
  int GetBookIndex(Book *book) {
    for (int i = 0; i < BookCount(); ++i) if (books[i] == book) return i;
    return -1;
  }
  void SetBrowserDirty(bool value) { dirty = value; }
};
static u64 now_ms = 2000, free_bytes = 32u * 1024u * 1024u;
static u64 osGetTime() { return now_ms; }
static const int MEMREGION_ALL = 0;
static u64 osGetMemRegionFree(int) { return free_bytes; }
static const uint8_t kCoverMaxAttempts = cover_cache::kMaxAttempts;
static void DrawBottomGradientFromApp(void *) {}
static void DrawTopGradientFromApp(void *) {}
static void DrawOpeningSplashWithProgress(unsigned, unsigned, void *) {}
LibraryController::LibraryController(App &app) : app_(app), inside_folder_(false) {}
void LibraryController::ResetBrowserMarquee() {}

// Extractor providers supply outcomes; the production job owner decides
// attempts, backoff, cache-save handling, redraw and queue completion.
static int extraction_rc = 0, extraction_backend = -1, extraction_calls = 0;
static bool extraction_pixels = false;
static int cover_saves = 0;
static Book *expected_extraction_book = nullptr;
static std::string expected_extraction_path;
static int ExtractOutcome(int backend, Book *book, const std::string &path) {
  assert(book == expected_extraction_book && !book->IsBrowserFolder());
  assert(path == expected_extraction_path);
  extraction_backend = backend;
  ++extraction_calls;
  if (extraction_pixels) {
    book->coverPixels = new u16[1]{0xf800};
    book->coverWidth = book->coverHeight = 1;
  }
  return extraction_rc;
}
namespace book_parser { uint8_t Index(Book *) { std::abort(); } }
#define OUTCOME_BACKEND(name, id) namespace name { \
  int ExtractCover(Book *book, const std::string &path) { return ExtractOutcome(id, book, path); } }
OUTCOME_BACKEND(epub_parser, 0)
OUTCOME_BACKEND(fb2_parser, 1)
OUTCOME_BACKEND(mobi_parser, 2)
OUTCOME_BACKEND(pdf_parser, 3)
OUTCOME_BACKEND(cbz_parser, 4)
namespace epub_parser { int ResolveToc(Book *, const std::string &) { std::abort(); } }
namespace cover_cache {
bool TryLoad(Book *, const std::string &) { std::abort(); }
bool Save(Book *book, const std::string &path) {
  assert(book == expected_extraction_book && !book->IsBrowserFolder());
  assert(path == expected_extraction_path && book->coverPixels);
  ++cover_saves;
  return false; // An unavailable disk cache must not undo a usable thumbnail.
}
bool TryLoadAdjacentOverride(Book *, const std::string &) { return false; }
}
#include "cover_jobs.inc"

static void Expect(bool condition, const char *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void TestEmbeddedCoverResults(const std::string &root) {
  const char *names[] = {"outcome.epub", "outcome.fb2", "outcome.mobi", "outcome.pdf", "outcome.cbz"};
  const format_t formats[] = {FORMAT_EPUB, FORMAT_XHTML, FORMAT_XHTML, FORMAT_PDF, FORMAT_CBZ};
  struct Outcome { int rc; bool pixels; int attempts; int pdf_attempts; bool old_retry; int saves; };
  const Outcome cases[] = {
      {0, true, 3, 3, false, 1}, {0, false, 3, 3, false, 0},
      {BOOK_ERR_CANCELLED, false, 1, 1, false, 0},
      {4, false, 2, 2, true, 0}, {1, false, 2, 2, true, 0},
      {-1, false, 2, 2, true, 0}, {3, false, 2, 3, true, 0},
      {8, false, 2, 3, true, 0}, {4, true, 2, 2, true, 0},
      {1, true, 2, 2, false, 0}};
  for (int backend = 0; backend < 5; ++backend) {
    for (bool new_device : {false, true}) {
      for (const Outcome &outcome : cases) {
        App app;
        app.new_3ds = new_device;
        Book *book = new Book(BookContext());
        app.books.push_back(book);
        app.selected = book;
        book->foldername = root;
        book->filename = names[backend];
        book->format = formats[backend];
        book->metadataIndexTried = true;
        book->coverImagePath = "embedded.png";
        book->coverAttempts = 1;
        book->coverRetryAfterMs = 99;
        now_ms = 10000;
        free_bytes = 32u * 1024u * 1024u;
        extraction_rc = outcome.rc;
        extraction_pixels = outcome.pixels;
        extraction_backend = -1;
        extraction_calls = cover_saves = 0;
        expected_extraction_book = book;
        expected_extraction_path = root + "/" + names[backend];
        LibraryController controller(app);
        controller.EnqueueJob(APP_JOB_EXTRACT_COVER, book);
        controller.ProcessJobs(3);
        Expect(extraction_calls == 1 && extraction_backend == backend,
               "cover job dispatches exactly one matching extractor");
        const int attempts = backend == 3 ? outcome.pdf_attempts : outcome.attempts;
        Expect(book->coverAttempts == attempts, "cover result preserves attempt policy");
        const bool retry = !new_device && outcome.old_retry && attempts < 3;
        const u64 expected_retry = outcome.rc == 0 && outcome.pixels
            ? 0 : (retry ? 12000u : 99u);
        Expect(book->coverRetryAfterMs == expected_retry,
               "success clears delay; errors/cancellation preserve existing backoff");
        Expect(cover_saves == outcome.saves, "only successful thumbnails reach the disk cache");
        Expect((book->coverPixels != nullptr) == outcome.pixels,
               "result handling preserves extractor-owned pixels");
        Expect(app.dirty, "visible completed cover job requests redraw");
        Expect(!controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, book),
               "result handler does not invent another queued attempt");
      }
    }
  }
  expected_extraction_book = nullptr;
  puts("PASS: embedded cover outcomes, cancellation, PDF permanent failures and cache-save failure");
}

int main() {
  TestEmbeddedCoverResults("/test/library");
}
