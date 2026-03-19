#pragma once

#include <list>

namespace layout_reflow {

int RemapPageIndexApprox(int old_index, int old_page_count, int new_page_count);
std::list<int> RemapBookmarksApprox(const std::list<int> &old_bookmarks,
                                    int old_page_count, int new_page_count);

} // namespace layout_reflow
