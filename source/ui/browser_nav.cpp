#include "ui/browser_nav.h"

namespace {

static int ClampInt(int value, int min_value, int max_value) {
  if (value < min_value)
    return min_value;
  if (value > max_value)
    return max_value;
  return value;
}

} // namespace

BrowserNavState BrowserNavMoveSelection(BrowserNavState state, int book_count,
                                        int page_size, int columns,
                                        BrowserNavMove move) {
  BrowserNavState out = state;
  if (book_count <= 0 || page_size <= 0 || columns <= 0) {
    out.selected_index = 0;
    out.page_start = 0;
    return out;
  }

  const int max_index = book_count - 1;
  int selected = ClampInt(state.selected_index, 0, max_index);
  int delta = 0;
  switch (move) {
  case BROWSER_NAV_LEFT:
    delta = -1;
    break;
  case BROWSER_NAV_RIGHT:
    delta = 1;
    break;
  case BROWSER_NAV_UP:
    delta = -columns;
    break;
  case BROWSER_NAV_DOWN:
    delta = columns;
    break;
  }

  // Move in library order and let the visible page follow the selection.
  selected = ClampInt(selected + delta, 0, max_index);
  out.selected_index = selected;
  out.page_start = (selected / page_size) * page_size;

  // Keep page_start in range even on short last pages.
  if (out.page_start > max_index)
    out.page_start = (max_index / page_size) * page_size;

  return out;
}
