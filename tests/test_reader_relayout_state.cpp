#include <cassert>
#include <cstdio>
#include <list>
#include <map>
#include "reader/app_book_internal.h"

class Book {
public:
  int pages = 80, position = 47;
  std::list<u16> bookmarks{12, 63};
  int GetPageCount() const { return pages; }
  int GetPosition() const { return position; }
  const std::list<u16> &GetBookmarks() const { return bookmarks; }
  void Close() { pages = 0; position = 0; bookmarks.clear(); }
};
namespace reader_internal {
#include "relayout_state_owner.inc"
}

int main() {
  using namespace reader_internal;
  Book first, second;
  const auto original = CaptureRelayoutState(&first, true);
  assert(original.needs_relayout && original.old_page_count == 80 &&
         original.old_position == 47 && original.old_bookmarks == std::list<int>({12, 63}));
  assert(first.GetPageCount() == 0);
  const auto retry = CaptureRelayoutState(&first, false);
  assert(retry.needs_relayout && retry.old_position == 47 && retry.old_page_count == 80);
  second.position = 20;
  CaptureRelayoutState(&second, true);
  const auto after_other_book = CaptureRelayoutState(&first, true);
  assert(after_other_book.old_position == 47 && after_other_book.old_bookmarks == original.old_bookmarks);
  ClearSavedRelayoutState(&second);
  assert(CaptureRelayoutState(&first, false).old_page_count == 80);
  ClearSavedRelayoutState(&first);
  const auto cleared = CaptureRelayoutState(&first, false);
  assert(!cleared.needs_relayout && cleared.old_page_count == 0 && cleared.old_bookmarks.empty());
  puts("PASS: cancelled relayout survives retries and other books, clears only its owner");
}
