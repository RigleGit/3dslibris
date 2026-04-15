#pragma once

class App;

namespace browser_list_view {

static const int kRowX = 5;
static const int kRowY0 = 4;
static const int kRowW = 230;
static const int kRowH = 36;
static const int kRowPitch = 38;

int HitTestBookIndex(int x, int y, int page_start, int book_count, int page_size);
void DrawPage(App &app, int page_start, int page_size);

} // namespace browser_list_view
