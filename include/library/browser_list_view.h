#pragma once

#include "library/browser_draw_context.h"

struct BrowserGridMarqueeState;

namespace browser_list_view {

static const int kRowX = 5;
static const int kRowY0 = 4;
static const int kRowW = 230;
static const int kRowH = 46;
static const int kRowPitch = 48;
// Side padding of the wrapped title; matches DrawWrappedTitleInsideCover.
static const int kTitlePadX = 8;
// Vertical padding of the wrapped title inside its row.
static const int kTitlePadY = 5;

int HitTestBookIndex(int x, int y, int page_start, int book_count, int page_size);
void DrawPage(const BrowserDrawContext &ctx, BrowserGridMarqueeState &marquee,
              int page_start, int page_size);

} // namespace browser_list_view
