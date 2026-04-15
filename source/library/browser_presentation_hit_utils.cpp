#include "library/browser_presentation_hit_utils.h"

namespace browser_presentation_hit_utils {

int HitTestGridBookIndex(int x, int y, int page_start, int book_count,
                         int grid_x0, int grid_y0, int cell_w, int cell_h,
                         int grid_cols, int grid_rows, int page_size) {
  if (x < grid_x0 || y < grid_y0)
    return -1;
  int col = (x - grid_x0) / cell_w;
  int row = (y - grid_y0) / cell_h;
  if (col < 0 || col >= grid_cols || row < 0 || row >= grid_rows)
    return -1;
  int page_idx = row * grid_cols + col;
  if (page_idx < 0 || page_idx >= page_size)
    return -1;
  int book_idx = page_start + page_idx;
  if (book_idx < 0 || book_idx >= book_count)
    return -1;
  return book_idx;
}

int HitTestListBookIndex(int x, int y, int page_start, int book_count,
                         int page_size, int row_x, int row_y0, int row_w,
                         int row_h, int row_pitch) {
  if (x < row_x || x >= row_x + row_w || y < row_y0 || page_size <= 0)
    return -1;
  int row = (y - row_y0) / row_pitch;
  int row_y = row_y0 + row * row_pitch;
  if (row < 0 || row >= page_size || y >= row_y + row_h)
    return -1;
  int book_idx = page_start + row;
  if (book_idx < 0 || book_idx >= book_count)
    return -1;
  return book_idx;
}

} // namespace browser_presentation_hit_utils
