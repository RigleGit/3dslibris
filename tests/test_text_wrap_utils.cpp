#include <string>
#include <vector>

#include "shared/text_wrap_utils.h"
#include "test_assert.h"

using namespace text_wrap_utils;

namespace test {
inline void ExpectStr(const char *label, const std::string &a, const std::string &b) {
  ExpectStrEq(label, a.c_str(), b.c_str());
}
}

// Host metric: every codepoint is 6px wide.
struct Metric { int width; };
static size_t Fit(const char *s, size_t len, void *ctx) {
  const int max_chars = ((Metric *)ctx)->width / 6;
  size_t bytes = 0;
  int chars = 0;
  while (bytes < len && chars < max_chars) {
    unsigned char c = (unsigned char)s[bytes];
    size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (bytes + n > len) n = len - bytes;
    bytes += n;
    chars++;
  }
  return bytes;
}

static std::vector<std::string> Wrap(const std::string &s, int width) {
  Metric m = {width};
  std::vector<Line> lines;
  WrapUtf8(s.c_str(), s.size(), Fit, &m, &lines);
  std::vector<std::string> out;
  for (size_t i = 0; i < lines.size(); i++)
    out.push_back(s.substr(lines[i].start, lines[i].end - lines[i].start));
  return out;
}

static std::string Join(const std::vector<std::string> &v) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++) r += (i ? "|" : "") + v[i];
  return r;
}

int main() {
  test::ExpectEq("empty", (int)Wrap("", 60).size(), 0);
  test::ExpectEq("spaces only", (int)Wrap("   ", 60).size(), 0);
  test::ExpectStr("short fits", Join(Wrap("abc def", 60)), "abc def");
  // 10 chars per line: break at spaces.
  test::ExpectStr("word wrap", Join(Wrap("hello wonderful world", 60)),
                  "hello|wonderful|world");
  test::ExpectStr("space at cut", Join(Wrap("abcdefghij klm", 60)), "abcdefghij|klm");
  // Unbreakable run is split between characters, nothing dropped.
  test::ExpectStr("unbreakable", Join(Wrap("WWWWWWWWWWWWWWWWWWWWWWW", 60)),
                  "WWWWWWWWWW|WWWWWWWWWW|WWW");
  test::ExpectStr("long word after short", Join(Wrap("ab WWWWWWWWWWWWWWW", 60)),
                  "ab|WWWWWWWWWW|WWWWW");
  // Multibyte: 2-byte and 3-byte characters are never split.
  test::ExpectStr("utf8 2byte", Join(Wrap("ÑÑÑÑÑÑÑÑÑÑÑÑ", 60)), "ÑÑÑÑÑÑÑÑÑÑ|ÑÑ");
  test::ExpectStr("utf8 3byte", Join(Wrap("日本語日本語日本語日本語", 36)),
                  "日本語日本語|日本語日本語");
  test::ExpectStr("arabic", Join(Wrap("كتاب تجريبي طويل", 66)), "كتاب تجريبي|طويل");
  // Width narrower than one glyph still progresses one character per line.
  test::ExpectEq("narrow", (int)Wrap("abc", 1).size(), 3);
  // Every non-space byte survives.
  std::string src = "The Extraordinarily Long Chapter Title That Cannot Possibly Fit";
  std::string joined;
  std::vector<std::string> w = Wrap(src, 90);
  for (size_t i = 0; i < w.size(); i++) joined += w[i];
  std::string expect;
  for (size_t i = 0; i < src.size(); i++) if (src[i] != ' ') expect += src[i];
  std::string got;
  for (size_t i = 0; i < joined.size(); i++) if (joined[i] != ' ') got += joined[i];
  test::ExpectStr("nothing dropped", got, expect);

  int h[] = {20, 30, 50, 10, 70};
  test::ExpectEq("fit all", RowsThatFit(h, 5, 0, 1000, 2), 5);
  test::ExpectEq("fit gap", RowsThatFit(h, 5, 0, 52, 2), 2);
  test::ExpectEq("fit tall row alone", RowsThatFit(h, 5, 4, 30, 2), 1);
  test::ExpectEq("fit past end", RowsThatFit(h, 5, 5, 30, 2), 0);
  std::vector<int> starts;
  PageStarts(h, 5, 60, 2, &starts);
  test::ExpectEq("pages", (int)starts.size(), 4);
  test::ExpectEq("page1", starts[1], 2);
  test::ExpectEq("page2", starts[2], 3);
  test::ExpectEq("page3", starts[3], 4);
  test::ExpectEq("page of 1", PageOfRow(starts, 1), 0);
  test::ExpectEq("page of 3", PageOfRow(starts, 3), 2);
  test::ExpectEq("page of 4", PageOfRow(starts, 4), 3);
  PageStarts(h, 0, 60, 2, &starts);
  test::ExpectEq("no rows", (int)starts.size(), 0);
  // Chapter-list layout: 3-line titles (52px) mixed with 1-line rows (34px)
  // between y=24 and the footer at 288; nothing may cross the bottom.
  {
    const int hs[] = {52, 34, 52, 34, 34, 52, 34, 34, 34, 34, 34, 34, 34, 34};
    std::vector<int> sizes, ys;
    LayoutRows(hs, 14, 24, 288, 2, &sizes, &ys);
    int total = 0;
    for (size_t p = 0; p < sizes.size(); p++) total += sizes[p];
    test::ExpectEq("every row is on a page", total, 14);
    test::ExpectEq("first row on top", ys[0], 24);
    test::ExpectEq("next row follows with a gap", ys[1], 24 + 52 + 2);
    int idx = 0;
    for (size_t p = 0; p < sizes.size(); p++) {
      for (int k = 0; k < sizes[p]; k++, idx++)
        test::ExpectTrue("row stays above the footer", ys[idx] + hs[idx] <= 288);
      if (p + 1 < sizes.size())
        test::ExpectTrue("page is full: next row would not fit",
                         ys[idx - 1] + hs[idx - 1] + 2 + hs[idx] > 288);
    }
    // A single row taller than the area still gets its own page.
    const int tall[] = {40, 400, 40};
    LayoutRows(tall, 3, 24, 288, 2, &sizes, &ys);
    test::ExpectEq("tall row pages", (int)sizes.size(), 3);
    LayoutRows(tall, 0, 24, 288, 2, &sizes, &ys);
    test::ExpectEq("empty list", (int)sizes.size(), 0);
  }
  printf("text_wrap_utils tests passed\n");
  return 0;
}
