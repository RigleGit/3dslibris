#pragma once

class App;
class Book;

struct BrowserGridMarqueeState {
  Book *book;
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
  bool active;

  BrowserGridMarqueeState();
  void Reset();
};

namespace browser_grid_view {

static const int kGridCols = 2;
static const int kGridRows = 2;
static const int kCoverW = 85;
static const int kCoverH = 115;
static const int kCellW = 115;
static const int kCellH = 144;
static const int kTitleOffsetY = kCoverH + 10;
static const int kProgressOffsetY = kCoverH + 22;
static const int kGridX0 = 5;
static const int kGridY0 = 3;

int HitTestBookIndex(int x, int y, int page_start, int book_count);
void DrawPage(App &app, BrowserGridMarqueeState &marquee, int page_start);
void TickMarquee(App &app, BrowserGridMarqueeState &marquee);

} // namespace browser_grid_view
