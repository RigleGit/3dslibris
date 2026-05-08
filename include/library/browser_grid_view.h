#pragma once

#include <string>

#include "book/cover_layout_constants.h"
#include "library/browser_draw_context.h"

class Book;

struct BrowserGridMarqueeState {
  Book *book;
  std::string display_name;
  unsigned short *strip;
  unsigned short *bg_strip;
  int strip_w;
  int strip_h;
  int strip_x;
  int strip_y;
  int blit_w;
  int scroll_offset;
  int scroll_timer;
  int end_timer;
  int color_mode;
  unsigned short bg_color;
  bool active;

  BrowserGridMarqueeState();
  void Reset();
};

namespace browser_grid_view {

static const int kGridCols = 2;
static const int kGridRows = 2;
static const int kPageCapacity = kGridCols * kGridRows;
static const int kCoverW = cover_layout::kBrowserCoverThumbWidth;
static const int kCoverH = cover_layout::kBrowserCoverThumbHeight;
static const int kCellW = 115;
static const int kCellH = 144;
static const int kTitleOffsetY = kCoverH + 10;
static const int kProgressOffsetY = kCoverH + 22;
static const int kGridX0 = 5;
static const int kGridY0 = 3;

int HitTestBookIndex(int x, int y, int page_start, int book_count);
void DrawPage(const BrowserDrawContext &ctx, BrowserGridMarqueeState &marquee, int page_start);
void TickMarquee(Text *ts, BrowserGridMarqueeState &marquee);

} // namespace browser_grid_view
