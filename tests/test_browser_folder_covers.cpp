// Exercise the actual directory admission, queue pump, sidecar I/O and decoder.
// App/Book below supply state and platform services, not cover policy.
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
#include "shared/cover_decode_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/home_button_guard.h"
#include "shared/image_scale_utils.h"
#include "shared/status_reporter.h"
#include "shared/stb_image_gif_utils.h"
#include "shared/string_utils.h"
#include "stb_image.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <new>
#include <set>
#include <sys/stat.h>
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

// None of these book-only backends may be reached by a folder cover job.
namespace book_parser { uint8_t Index(Book *) { std::abort(); } }
#define UNUSED_BACKEND(name) namespace name { \
  int ExtractCover(Book *, const std::string &) { std::abort(); } }
UNUSED_BACKEND(epub_parser)
UNUSED_BACKEND(fb2_parser)
UNUSED_BACKEND(mobi_parser)
UNUSED_BACKEND(pdf_parser)
UNUSED_BACKEND(cbz_parser)
namespace epub_parser { int ResolveToc(Book *, const std::string &) { std::abort(); } }
namespace cover_cache {
bool TryLoad(Book *, const std::string &) { std::abort(); }
bool Save(Book *, const std::string &) { std::abort(); }
#include "folder_sidecar.inc"
}
static int decode_calls = 0;
namespace cover_decode_utils {
#define DecodeImageToCoverThumb DecodeForFolderTest
#include "folder_decode.inc"
#undef DecodeImageToCoverThumb
bool DecodeImageToCoverThumb(Book *book, const unsigned char *data, size_t size, int cap) {
  ++decode_calls;
  return DecodeForFolderTest(book, data, size, cap);
}
}
#include "folder_book.inc"
#include "folder_index.inc"
#include "folder_jobs.inc"
#include "folder_covers.inc"

static void Expect(bool condition, const char *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static int LoadedCount(const App &app) {
  int count = 0;
  for (Book *book : app.books) if (book->coverPixels) ++count;
  return count;
}
static void ExpectColor(const Book *book, u16 color) {
  Expect(book->coverPixels && book->coverWidth == 2 && book->coverHeight == 2,
         "original sidecar dimensions are preserved");
  for (int i = 0; i < 4; ++i)
    Expect(book->coverPixels[i] == color, "sidecar RGB565 pixels match");
}
static void LoadPage(LibraryController &controller, App &app) {
  for (int i = 0; i < 8; ++i) {
    controller.TickBrowserWarmup();
    const int before = decode_calls;
    controller.ProcessJobs(3);
    Expect(decode_calls - before <= 1, "browser returns after one decode job");
  }
  Expect(LoadedCount(app) == 4, "only the four visible folder covers are resident");
}
int main() {
  const std::string root = std::getenv("TEST_FOLDER_COVERS_DIR");
  App app;
  LibraryController controller(app);
  std::set<std::string> seen;
  for (int i = 0; i < 128; ++i) {
    char name[32]; snprintf(name, sizeof(name), "Series.%03d", i);
    AppendFolderEntry(&app, root, name, &seen);
  }
  AppendFolderEntry(&app, root, "Series.000", &seen);
  AppendFolderEntry(&app, root, "empty", &seen);
  AppendFolderEntry(&app, root, "unsupported", &seen);
  Expect(app.BookCount() == 128, "directory admission and deduplication preserved");
  Expect(decode_calls == 0 && LoadedCount(app) == 0, "indexing never decodes folder covers");
  Expect(app.books[0]->title == "Series.000", "folder label preserved");
  app.selected = app.books[0];
  app.last_interaction = now_ms;
  controller.LoadVisibleBrowserCoverCaches();
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(decode_calls == 0, "no decode during browser interaction");
  now_ms += 250;
  controller.TickBrowserWarmup();
  Expect(controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, app.selected), "selected folder queues after idle");
  Expect(!controller.HasQueuedJob(APP_JOB_INDEX_METADATA, app.selected), "folder never queues metadata");
  app.suspended = true; controller.ProcessJobs(3);
  Expect(decode_calls == 0, "suspend defers queued cover");
  app.suspended = false; app.abort_work = true; controller.ProcessJobs(3);
  Expect(decode_calls == 0, "abort defers queued cover");
  app.abort_work = false; app.input_release = true; controller.ProcessJobs(3);
  Expect(decode_calls == 0, "held input defers queued cover");
  app.input_release = false; controller.ProcessJobs(3);
  Expect(decode_calls == 1 && LoadedCount(app) == 1 && app.dirty, "selected folder decodes and redraws");
  ExpectColor(app.selected, 0xf800);
  now_ms += 1200; LoadPage(controller, app);
  Expect(decode_calls == 4, "first page decodes each folder once");
  app.page_start = 4; app.selected = app.books[4];
  controller.LoadVisibleBrowserCoverCaches();
  Expect(LoadedCount(app) == 0, "leaving page evicts folder covers");
  for (int i = 0; i < 4; ++i)
    Expect(app.books[i]->coverWidth == 0 && app.books[i]->coverHeight == 0,
           "eviction clears dimensions");
  LoadPage(controller, app);
  for (int start = 8; start < 128; start += 4) {
    app.page_start = start; app.selected = app.books[start];
    controller.LoadVisibleBrowserCoverCaches(); LoadPage(controller, app);
  }
  // Change the sidecar's extension and bytes while the folder is offscreen.
  Expect(std::remove((root + "/Series.000.png").c_str()) == 0, "remove previous sidecar");
  Expect(std::rename((root + "/blue.png").c_str(), (root + "/Series.000.PNG").c_str()) == 0,
         "replace sidecar");
  app.page_start = 0; app.selected = app.books[0];
  controller.LoadVisibleBrowserCoverCaches(); LoadPage(controller, app);
  ExpectColor(app.selected, 0x001f);
  // Stale queued work must be consumed without decoding even if a caller
  // changes the page directly, before the regular visible-cache refresh.
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, app.books[8]);
  const int before = decode_calls;
  controller.ProcessJobs(3);
  Expect(decode_calls == before && !controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, app.books[8]),
         "stale offscreen folder job is discarded");
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, app.books[1]);
  app.prefs->browser_view_mode = BROWSER_VIEW_LIST;
  controller.LoadVisibleBrowserCoverCaches();
  Expect(LoadedCount(app) == 0 && !controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, app.books[1]),
         "list mode evicts thumbnails and prunes folder jobs");
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(decode_calls == before, "list view never decodes covers");
  app.prefs->browser_view_mode = BROWSER_VIEW_GALLERY;
  app.last_interaction = now_ms;
  controller.LoadVisibleBrowserCoverCaches();
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(decode_calls == before, "view switch still respects idle gate");
  now_ms += 2000;
  // Keep a normal book job while pruning invisible folder jobs.
  Book *ordinary = new Book(BookContext()); app.books.push_back(ordinary);
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, ordinary);
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, app.books[8]);
  controller.UnloadNonVisibleBrowserCoverCaches();
  Expect(controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, ordinary) &&
         !controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, app.books[8]),
         "folder pruning preserves ordinary book jobs");
  Expect(controller.PauseBrowserJobs() == 1, "existing pause clears remaining heavy job");
  free_bytes = 0;
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(decode_calls == before && app.selected->coverRetryAfterMs > now_ms,
         "existing low memory admission defers decode");
  free_bytes = 32u * 1024u * 1024u; now_ms += 3000;
  LoadPage(controller, app); ExpectColor(app.selected, 0x001f);
  // A missing or corrupt sidecar finishes this visit without busy retries,
  // but leaving and returning must allow a newly repaired sidecar.
  app.page_start = 8; app.selected = app.books[8];
  Expect(std::remove((root + "/Series.008.png").c_str()) == 0, "remove missing fixture");
  FILE *broken = fopen((root + "/Series.009.png").c_str(), "wb");
  Expect(broken != nullptr, "open corrupt fixture");
  fputs("not an image", broken); fclose(broken);
  controller.LoadVisibleBrowserCoverCaches();
  for (int i = 0; i < 8; ++i) { controller.TickBrowserWarmup(); controller.ProcessJobs(3); }
  Expect(!app.books[8]->coverPixels && !app.books[9]->coverPixels && LoadedCount(app) == 2,
         "missing and corrupt sidecars leave placeholders");
  const int failed_visit = decode_calls;
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(decode_calls == failed_visit, "failed sidecar does not decode repeatedly");
  app.page_start = 0; app.selected = app.books[0]; controller.LoadVisibleBrowserCoverCaches();
  for (int index : {8, 9}) {
    char name[40]; snprintf(name, sizeof(name), "/Series.%03d.png", index);
    std::string bytes;
    Expect(file_read_utils::ReadPathToStringLimited(root + "/Series.010.png", &bytes, 4096), "read repaired image");
    FILE *out = fopen((root + name).c_str(), "wb");
    Expect(out && fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size(), "repair sidecar");
    fclose(out);
  }
  app.page_start = 8; app.selected = app.books[8];
  controller.LoadVisibleBrowserCoverCaches(); LoadPage(controller, app);
  ExpectColor(app.books[8], 0xf800); ExpectColor(app.books[9], 0xf800);
  // New 3DS can admit several visible jobs; selection still takes priority.
  app.new_3ds = true; app.page_start = 12; app.selected = app.books[13];
  controller.UnloadNonVisibleBrowserCoverCaches();
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, app.books[12]);
  controller.EnqueueJob(APP_JOB_EXTRACT_COVER, app.books[13]);
  controller.PrioritizeSelectedBookJobs(app.selected); controller.ProcessJobs(3);
  Expect(app.books[13]->coverPixels && !app.books[12]->coverPixels,
         "selected folder runs first on a multi-job queue");
  controller.TickBrowserWarmup();
  const int before_mode = decode_calls;
  app.mode = AppMode::Book; controller.ProcessJobs(3);
  Expect(decode_calls == before_mode && !controller.HasQueuedJob(APP_JOB_EXTRACT_COVER, app.books[12]),
         "leaving browser discards pending folder work");
  puts("PASS: folder indexing, deferred visible covers, eviction and sidecar refresh");
}
