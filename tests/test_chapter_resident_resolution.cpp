#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
typedef uint16_t u16;
typedef uint64_t u64;
static u64 clock_ms = 0;
static u64 osGetTime() {
  return ++clock_ms;
}
#define DBG_LOGF(...) ((void)0)
#define DBG_LOG(...) ((void)0)
enum TocQuality {
  TOC_QUALITY_UNKNOWN,
  TOC_QUALITY_STRONG,
  TOC_QUALITY_MIXED,
  TOC_QUALITY_HEURISTIC
};
struct Book {
  bool resident = true;
  const char *name = "test.mobi";
  unsigned session = 10, target = 0, ready = 0, ticks = 0, reads = 0;
  std::vector<unsigned> requested;
  bool HasReflowPageCache() {
    return resident;
  }
  const char *GetFileName() {
    return name;
  }
  TocQuality GetTocQuality() {
    return TOC_QUALITY_MIXED;
  }
  unsigned GetOpenSessionId() {
    return session;
  }
  unsigned GetPageCount() {
    return 600;
  }
  bool IsReflowPageReady(unsigned p) {
    return p == ready;
  }
  bool RequestReflowPage(unsigned p) {
    if (target != p) {
      target = p;
      ticks = 0;
      requested.push_back(p);
    }
    return true;
  }
  int AdvanceReflowPageCache() {
    ++reads;
    if (++ticks < 2)
      return 1;
    ready = target;
    return 0;
  }
  void TrimReflowPageCache() {}
};
static bool HasExtCI(const char *name, const char *ext) {
  return std::string(name).find(ext) != std::string::npos;
}
static std::string NormalizeSearchText(const std::string &s) {
  return s;
}
static unsigned matching_page = 400;
static void EvaluateApproxPage(Book *, u16 p, const std::string &, u16,
                               int *score, u16 *best) {
  if (p == matching_page) {
    *score = 500;
    *best = p;
  }
}
static bool FindApproxTitlePage(Book *, const std::string &, u16, u16 *) {
  std::abort();
}
struct PagedListMenu {
  std::vector<u16> hints = {420};
  bool ResolveTargetPage(u16 i, u16 *out) {
    *out = hints[i];
    return true;
  }
};
struct ChapterMenu : PagedListMenu {
  Book *current_book_;
  void *app = nullptr, *status_reporter = nullptr;
  bool approx_pending_ = false;
  unsigned approx_index_ = 0, approx_cursor_ = 0, approx_end_ = 0,
           approx_phase_ = 0, approx_session_ = 0;
  int approx_best_score_ = -100000;
  u16 approx_best_page_ = 0;
  std::string approx_query_;
  std::vector<std::string> entry_titles = {"Chapter test"};
  std::vector<u16> entry_pages = {420};
  std::vector<int> approx_page_cache = {-1};
  bool ResolveTargetPage(u16, u16 *);
};
#include "chapter_resolution_owner.inc"
int main() {
  Book book;
  ChapterMenu menu;
  menu.current_book_ = &book;
  u16 page = 0;
  assert(!menu.ResolveTargetPage(0, &page));
  assert(menu.approx_pending_ && !book.requested.empty());
  unsigned frames = 0;
  while (!menu.ResolveTargetPage(0, &page))
    assert(++frames < 1000);
  assert(page == 400 && !menu.approx_pending_ && frames > 0);
  assert(*std::max_element(book.requested.begin(), book.requested.end()) < 500);
  const unsigned reads = book.reads;
  assert(menu.ResolveTargetPage(0, &page) && page == 400 &&
         reads == book.reads);
  // EPUB's resolved targets must keep their exact, immediate behavior.
  book.name = "test.epub";
  menu.approx_page_cache[0] = -1;
  assert(menu.ResolveTargetPage(0, &page) && page == 420 &&
         reads == book.reads);
  // The original trust bound still rejects a title too far from its hint.
  book.name = "test.mobi";
  matching_page = 470;
  menu.approx_page_cache[0] = -1;
  while (!menu.ResolveTargetPage(0, &page))
    assert(++frames < 2000);
  assert(page == 420 && menu.approx_page_cache[0] == -2);
}
