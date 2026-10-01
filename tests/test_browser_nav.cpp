#include "ui/browser_nav.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_view_utils.h"
#include "library/browser_folder_input_utils.h"
#include "library/browser_presentation_hit_utils.h"
#include "library/browser_grid_view.h"
#include "library/browser_list_view.h"
#include "library/cover_cache.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <cstdlib>
#include <string>

struct Book {
  std::string folder="books", filename="book.epub";
  int id=0; bool is_folder=false, missing_name=false;
  uint16_t *coverPixels=nullptr; int coverWidth=0, coverHeight=0;
  uint8_t coverAttempts=0; uint64_t coverRetryAfterMs=99;
  bool IsBrowserFolder() const { return is_folder; }
  const char *GetFolderName() const { return folder.c_str(); }
  const char *GetFileName() const { return missing_name ? nullptr : filename.c_str(); }
  ~Book() { delete[] coverPixels; }
};
struct Prefs { BrowserViewMode browser_view_mode=BROWSER_VIEW_GALLERY; };
struct App {
  Prefs settings; Prefs *prefs=&settings; std::vector<Book *> books;
  Book *selected=nullptr; int page_start=0; bool dirty=false; uint64_t interaction_ms=0;
  int BookCount() const { return (int)books.size(); }
  int GetBrowserPageStart() const { return page_start; }
  void SetBrowserPageStart(int v) { page_start=v; }
  Book *GetSelectedBook() const { return selected; }
  void SetSelectedBook(Book *v) { selected=v; }
  void SetBrowserLastInteractionMs(uint64_t v) { interaction_ms=v; }
  void SetBrowserDirty(bool v) { dirty=v; }
};
static std::vector<int> loaded;
namespace cover_cache {
bool TryLoad(Book *book, const std::string &path) {
  assert(path == book->folder + "/" + book->filename);
  loaded.push_back(book->id);
  book->coverPixels=new uint16_t[4]; book->coverWidth=book->coverHeight=2;
  return true;
}
}
static uint64_t osGetTime() { return 1234; }
static const uint8_t kCoverMaxAttempts=cover_cache::kMaxAttempts;
static struct Marquee { int resets=0; void Reset() { ++resets; } } g_marquee;
struct LibraryController {
  App &app_; Book *prioritized=nullptr;
  explicit LibraryController(App &app): app_(app) {}
  void PrioritizeSelectedBookJobs(Book *book) { prioritized=book; }
  void LoadVisibleBrowserCoverCaches();
  void UnloadNonVisibleBrowserCoverCaches();
  void browser_nextpage(); void browser_prevpage();
};
#include "browser_navigation_under_test.inc"
namespace browser_grid_view {
#include "browser_grid_hit_under_test.inc"
}
namespace browser_list_view {
#include "browser_list_hit_under_test.inc"
}

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectState(const char *label, BrowserNavState actual, int selected,
                 int page_start) {
  if (actual.selected_index != selected || actual.page_start != page_start) {
    Fail(std::string(label) + ": unexpected browser navigation state");
  }
}

} // namespace

static void TestBrowserPagesLoadOnlyVisibleCovers() {
  Book storage[10]; App app;
  for (int i=0; i<10; ++i) { storage[i].id=i; app.books.push_back(&storage[i]); }
  storage[2].is_folder=true; storage[3].missing_name=true; app.books[5]=nullptr;
  LibraryController controller(app);
  loaded.clear(); controller.LoadVisibleBrowserCoverCaches();
  assert((loaded == std::vector<int>{0, 1}));
  for (int i : {0, 1}) assert(storage[i].coverAttempts == 3 && storage[i].coverRetryAfterMs == 0);
  controller.LoadVisibleBrowserCoverCaches();
  assert(loaded.size() == 2); // Already loaded visible covers are reused.
  loaded.clear(); controller.browser_nextpage();
  assert(app.page_start == 4 && app.selected == &storage[4]);
  assert(controller.prioritized == app.selected && app.dirty && app.interaction_ms == 1234);
  assert((loaded == std::vector<int>{4, 6, 7}));
  assert(!storage[0].coverPixels && !storage[1].coverPixels && storage[0].coverWidth == 0 && storage[0].coverHeight == 0);
  loaded.clear(); controller.browser_nextpage();
  assert(app.page_start == 8 && app.selected == &storage[8]);
  assert((loaded == std::vector<int>{8, 9})); // Short last page stays bounded.
  loaded.clear(); app.dirty=false; const int resets=g_marquee.resets;
  controller.browser_nextpage();
  assert(app.page_start == 8 && loaded.empty() && !app.dirty && g_marquee.resets == resets);
  controller.browser_prevpage();
  assert(app.page_start == 4 && app.selected == &storage[7]);
  assert(!storage[8].coverPixels && !storage[9].coverPixels);
  controller.browser_prevpage(); assert(app.page_start == 0);
  app.settings.browser_view_mode=BROWSER_VIEW_LIST;
  loaded.clear(); controller.LoadVisibleBrowserCoverCaches();
  assert(loaded.empty());
  for (auto &book : storage) assert(!book.coverPixels);
  controller.browser_nextpage();
  assert(app.page_start == 7 && app.selected == &storage[7] && loaded.empty());
  controller.browser_prevpage();
  assert(app.page_start == 0 && app.selected == &storage[6]);
  app.books.clear(); controller.LoadVisibleBrowserCoverCaches();
  controller.browser_nextpage(); controller.browser_prevpage();
}
static void TestBrowserPresentationAndStoredPolicy() {
  // Use public wrappers and their production geometry, not copied dimensions.
  using namespace browser_grid_view;
  assert(HitTestBookIndex(kGridX0 + 1, kGridY0 + 1, 4, 6) == 4);
  assert(HitTestBookIndex(kGridX0 + kCellW + 1, kGridY0 + 1, 4, 6) == 5);
  assert(HitTestBookIndex(kGridX0 + 1, kGridY0 + kCellH + 1, 4, 6) == -1);
  assert(HitTestBookIndex(kGridX0 - 1, kGridY0, 0, 8) == -1);
  assert(HitTestBookIndex(kGridX0, kGridY0 + kCellH * kGridRows, 0, 8) == -1);
  using browser_list_view::kRowX; using browser_list_view::kRowY0;
  using browser_list_view::kRowW; using browser_list_view::kRowH; using browser_list_view::kRowPitch;
  const int page_size=browser_view_utils::PageSize(BROWSER_VIEW_LIST);
  assert(browser_list_view::HitTestBookIndex(kRowX+1, kRowY0+1, 7, 10, page_size) == 7);
  assert(browser_list_view::HitTestBookIndex(kRowX+1, kRowY0+kRowPitch+1, 7, 10, page_size) == 8);
  assert(browser_list_view::HitTestBookIndex(kRowX+1, kRowY0+kRowPitch*3+1, 7, 10, page_size) == -1);
  assert(browser_list_view::HitTestBookIndex(kRowX+1, kRowY0+kRowH, 7, 10, page_size) == -1);
  assert(browser_list_view::HitTestBookIndex(kRowX+kRowW, kRowY0+1, 7, 10, page_size) == -1);
  // Navigation consumes the actual view policy: one list row versus one
  // gallery row, including the corresponding page size.
  for (BrowserViewMode mode : {BROWSER_VIEW_GALLERY, BROWSER_VIEW_LIST}) {
    const auto state=BrowserNavMoveSelection({0, 0}, 10,
        browser_view_utils::PageSize(mode), browser_view_utils::ColumnCount(mode), BROWSER_NAV_DOWN);
    assert(state.selected_index == (mode == BROWSER_VIEW_LIST ? 1 : 2));
    assert(state.page_start == 0);
  }
  for (const char *value : {static_cast<const char *>(nullptr), "weird", "gallery"})
    assert(browser_view_utils::ParsePrefValue(value) == BROWSER_VIEW_GALLERY);
  assert(browser_view_utils::ParsePrefValue("list") == BROWSER_VIEW_LIST);
  assert(std::string(browser_view_utils::ToPrefValue(BROWSER_VIEW_LIST)) == "list");
  assert(std::string(browser_view_utils::ToPrefValue(BROWSER_VIEW_GALLERY)) == "gallery");
  assert(std::string(browser_view_utils::Label(BROWSER_VIEW_GALLERY)) == "Gallery");
  assert(std::string(browser_view_utils::Label(BROWSER_VIEW_LIST)) == "List");
  assert(browser_view_utils::ListTitleMaxLines() == 2 && browser_view_utils::ListTitleBoxHeight(10) == 32);
  for (int color=0; color<6; ++color) {
    const auto selected=browser_view_utils::PaletteForListRow(true, color);
    const auto normal=browser_view_utils::PaletteForListRow(false, color);
    assert(selected.text != selected.fill && normal.text != normal.fill);
    assert(selected.fill != normal.fill);
  }
  // Folder input remains a focused admission contract; no directory I/O is
  // modeled or claimed by these mask checks.
  const uint32_t back=1, start=2, select=4;
  for (uint32_t key : {back, start, back|start})
    assert(browser_folder_input_utils::ShouldLeaveFolder(key, back, start));
  assert(!browser_folder_input_utils::ShouldLeaveFolder(select, back, start));
  assert(!browser_folder_input_utils::ShouldLeaveFolder(0, back, start));
  // The redraw admission guard remains meaningful for offscreen job results.
  const auto visible=browser_cover_cache_utils::ComputeVisibleRange(4, 10, 4);
  assert(browser_cover_cache_utils::VisibleBookNeedsBrowserRedraw(visible, 4));
  assert(!browser_cover_cache_utils::VisibleBookNeedsBrowserRedraw(visible, 8));
}
int main() {
  TestBrowserPagesLoadOnlyVisibleCovers();
  TestBrowserPresentationAndStoredPolicy();
  BrowserNavState state{0, 0};

  ExpectState("moves right inside page",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_RIGHT), 1,
              0);
  ExpectState("moves down by one row",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_DOWN), 2, 0);
  ExpectState("clamps at first item on left",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_LEFT), 0, 0);
  ExpectState("jumps to next page when moving past page boundary",
              BrowserNavMoveSelection({3, 0}, 8, 4, 2, BROWSER_NAV_RIGHT), 4,
              4);
  ExpectState("keeps same column when moving to next page row",
              BrowserNavMoveSelection({2, 0}, 8, 4, 2, BROWSER_NAV_DOWN), 4,
              4);
  ExpectState("clamps on short last page",
              BrowserNavMoveSelection({6, 4}, 7, 4, 2, BROWSER_NAV_RIGHT), 6,
              4);
  ExpectState("list view moves down by one item",
              BrowserNavMoveSelection({0, 0}, 10, 7, 1, BROWSER_NAV_DOWN), 1,
              0);
  ExpectState("list view moves up by one item",
              BrowserNavMoveSelection({5, 0}, 10, 7, 1, BROWSER_NAV_UP), 4, 0);
  ExpectState("list view advances page at boundary",
              BrowserNavMoveSelection({6, 0}, 10, 7, 1, BROWSER_NAV_DOWN), 7,
              7);
  ExpectState("list view clamps horizontal movement",
              BrowserNavMoveSelection({4, 0}, 10, 7, 1, BROWSER_NAV_RIGHT), 5,
              0);
  ExpectState("handles empty library",
              BrowserNavMoveSelection({5, 4}, 0, 4, 2, BROWSER_NAV_RIGHT), 0,
              0);

  return 0;
}
