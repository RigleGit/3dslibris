#pragma once

struct BrowserNavState {
  int selected_index;
  int page_start;
};

enum BrowserNavMove {
  BROWSER_NAV_LEFT,
  BROWSER_NAV_RIGHT,
  BROWSER_NAV_UP,
  BROWSER_NAV_DOWN
};

BrowserNavState BrowserNavMoveSelection(BrowserNavState state, int book_count,
                                        int page_size, int columns,
                                        BrowserNavMove move);
