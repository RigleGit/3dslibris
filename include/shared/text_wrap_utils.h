#pragma once

#include <cstddef>
#include <vector>

// Pure word-wrap and row-pagination helpers shared by list-style UI. Nothing
// here drops text: every byte of the input lands in exactly one line.
namespace text_wrap_utils {

// Returns how many bytes of s[0, len) fit in one line. The result must end on a
// UTF-8 character boundary; 0 means not even one character fits.
typedef size_t (*FitFn)(const char *s, size_t len, void *ctx);

struct Line {
  size_t start; // first byte of the line
  size_t end;   // one past the last byte, trailing spaces excluded
};

// Splits s into lines. Breaks at spaces when possible; a run without spaces
// that is wider than a line is split between characters. Always makes progress,
// so a width narrower than one glyph yields one character per line.
void WrapUtf8(const char *s, size_t len, FitFn fit, void *ctx,
              std::vector<Line> *out);

// Number of rows starting at `start` whose heights (plus `gap` between rows)
// fit in `avail` pixels. Returns at least 1 while start < count so that a row
// taller than the page is still reachable.
int RowsThatFit(const int *heights, int count, int start, int avail, int gap);

// First row index of every page when rows are paginated by cumulative height.
void PageStarts(const int *heights, int count, int avail, int gap,
                std::vector<int> *out);

// Index in `starts` of the page holding row `index`.
int PageOfRow(const std::vector<int> &starts, int index);

} // namespace text_wrap_utils
