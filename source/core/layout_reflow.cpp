#include "layout_reflow.h"

#include <set>

namespace layout_reflow {

namespace {

int ClampPageIndex(int index, int page_count) {
  if (page_count <= 0)
    return 0;
  if (index < 0)
    return 0;
  if (index >= page_count)
    return page_count - 1;
  return index;
}

} // namespace

int RemapPageIndexApprox(int old_index, int old_page_count, int new_page_count) {
  if (new_page_count <= 0)
    return 0;

  if (old_page_count <= 1)
    return ClampPageIndex(old_index, new_page_count);

  const int clamped_old = ClampPageIndex(old_index, old_page_count);
  const int old_last = old_page_count - 1;
  const int new_last = new_page_count - 1;
  const long long numer =
      (long long)clamped_old * (long long)new_last + (old_last / 2);
  const int mapped = (int)(numer / old_last);
  return ClampPageIndex(mapped, new_page_count);
}

std::list<int> RemapBookmarksApprox(const std::list<int> &old_bookmarks,
                                    int old_page_count, int new_page_count) {
  std::set<int> remapped;
  for (int bookmark : old_bookmarks) {
    remapped.insert(
        RemapPageIndexApprox(bookmark, old_page_count, new_page_count));
  }
  return std::list<int>(remapped.begin(), remapped.end());
}

} // namespace layout_reflow
