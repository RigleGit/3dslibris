#include "shared/text_wrap_utils.h"

namespace text_wrap_utils {

namespace {

size_t CharLen(const char *s, size_t len) {
  unsigned char c = (unsigned char)s[0];
  size_t n = 1;
  if ((c & 0xE0) == 0xC0)
    n = 2;
  else if ((c & 0xF0) == 0xE0)
    n = 3;
  else if ((c & 0xF8) == 0xF0)
    n = 4;
  if (n > len)
    n = len;
  for (size_t i = 1; i < n; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80)
      return i;
  }
  return n;
}

size_t BackToBoundary(const char *s, size_t pos, size_t take) {
  while (take > 0 && ((unsigned char)s[pos + take] & 0xC0) == 0x80)
    take--;
  return take;
}

} // namespace

void WrapUtf8(const char *s, size_t len, FitFn fit, void *ctx,
              std::vector<Line> *out) {
  out->clear();
  size_t pos = 0;
  while (pos < len) {
    while (pos < len && s[pos] == ' ')
      pos++;
    if (pos >= len)
      break;
    const size_t rest = len - pos;
    size_t take = fit(s + pos, rest, ctx);
    if (take > rest)
      take = rest;
    size_t cut;
    if (take >= rest) {
      cut = rest;
    } else {
      take = BackToBoundary(s, pos, take);
      if (take == 0)
        take = CharLen(s + pos, rest);
      cut = take;
      if (s[pos + take] != ' ') {
        // Prefer the last space inside the fitted text; otherwise the run is
        // unbreakable and is split between characters.
        for (size_t k = take; k > 0; k--) {
          if (s[pos + k - 1] == ' ') {
            cut = k - 1;
            break;
          }
        }
        if (cut == 0)
          cut = take;
      }
    }
    size_t end = pos + cut;
    while (end > pos && s[end - 1] == ' ')
      end--;
    Line line;
    line.start = pos;
    line.end = end;
    out->push_back(line);
    pos += cut;
  }
}

int RowsThatFit(const int *heights, int count, int start, int avail, int gap) {
  if (start < 0 || start >= count)
    return 0;
  int used = 0;
  int n = 0;
  for (int i = start; i < count; i++) {
    const int h = heights[i] + (n ? gap : 0);
    if (n && used + h > avail)
      break;
    used += h;
    n++;
  }
  return n;
}

void PageStarts(const int *heights, int count, int avail, int gap,
                std::vector<int> *out) {
  out->clear();
  int start = 0;
  while (start < count) {
    out->push_back(start);
    start += RowsThatFit(heights, count, start, avail, gap);
  }
}

int PageOfRow(const std::vector<int> &starts, int index) {
  int page = 0;
  for (size_t i = 0; i < starts.size(); i++) {
    if (starts[i] <= index)
      page = (int)i;
  }
  return page;
}

} // namespace text_wrap_utils
