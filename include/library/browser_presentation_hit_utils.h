#pragma once

namespace browser_presentation_hit_utils {

int HitTestGridBookIndex(int x, int y, int page_start, int book_count,
                         int grid_x0, int grid_y0, int cell_w, int cell_h,
                         int grid_cols, int grid_rows, int page_size);
int HitTestListBookIndex(int x, int y, int page_start, int book_count,
                         int page_size, int row_x, int row_y0, int row_w,
                         int row_h, int row_pitch);

} // namespace browser_presentation_hit_utils
