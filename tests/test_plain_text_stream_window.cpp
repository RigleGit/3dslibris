#include "formats/common/plain_text_stream.h"
#include "book/book.h"
#include "book/book_xml.h"
#include "shared/text_token_constants.h"
#include "test_assert.h"

#include <string>
#include <vector>

// Recording stand-ins for the page layout: every chardata call is logged and
// a new "page" starts every kPageBytes of fed text.
namespace {
std::string g_log;
unsigned g_pages = 0;
size_t g_fed_since_page = 0;
const size_t kPageBytes = 90;
unsigned g_seed = 4242;
unsigned Rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 8) & 0xffffff;
}
} // namespace

u16 Book::GetPageCount() { return (u16)g_pages; }
void xml::book::chardata(void *, const char *txt, int len) {
  g_log += std::string(txt, (size_t)len);
  g_log += '|';
  g_fed_since_page += (size_t)len;
  while (g_fed_since_page >= kPageBytes) {
    g_fed_since_page -= kPageBytes;
    ++g_pages;
  }
}
namespace text_layout_utils {
PerfStats GetPerfStats() { return PerfStats(); }
}

namespace {
bool IsBlank(const std::string &l) { return l.find_first_not_of(" \t") == std::string::npos; }
bool LooksHeading(const std::string &l, bool *strong) {
  *strong = l.size() < 12 && !l.empty() && l[0] == 'H';
  return l.size() < 30 && !l.empty() && l[0] == 'H';
}
bool Accept(const std::string &, bool prev_blank, bool next_blank, bool, bool next_candidate, bool strong) {
  return strong || (prev_blank && next_blank) || next_candidate;
}
void AddChapter(Book *, const std::string &t, u8) { g_log += "[ch:" + t + "]"; }
bool Keep(parsedata_t *, int level) { g_log += "[h" + std::to_string(level) + "]"; return true; }
void Image(parsedata_t *, u16 id, InlineImageContext) { g_log += "[img" + std::to_string(id) + "]"; }
void Finalize(parsedata_t *) { g_log += "[fin]"; }
void Confidence(Book *, bool) {}

plain_text_stream::ContinueCallbacks Callbacks() {
  plain_text_stream::ContinueCallbacks c;
  c.is_blank_line = IsBlank;
  c.looks_like_plain_chapter_heading = LooksHeading;
  c.should_accept_heuristic_heading = Accept;
  c.add_chapter_if_unique = AddChapter;
  c.apply_heading_keep_with_next = Keep;
  c.append_inline_image_to_parsedata = Image;
  c.finalize_plain_page = Finalize;
  c.set_non_epub_toc_confidence = Confidence;
  return c;
}

std::string RandomLines(size_t lines) {
  static const char *const words[] = {"alpha", "Heading", "beta ", "H1", "  ", "\t", "gamma delta", "caf\xc3\xa9"};
  std::string out;
  for (size_t i = 0; i < lines; ++i) {
    const unsigned kind = Rand() % 9;
    if (kind == 0) { // empty line
    } else if (kind == 1) { // image token
      out += (char)TEXT_IMAGE_LEADING_PARAGRAPH;
      out += (char)TEXT_IMAGE;
      out += (char)0;
      out += (char)(Rand() % 200 + 1);
      out += "tail";
    } else if (kind == 2) { // heading marker
      out += (char)0x1e;
      out += "title";
    } else {
      const unsigned n = Rand() % 12;
      for (unsigned w = 0; w < n; ++w)
        out += words[Rand() % 8];
    }
    if (i + 1 < lines || Rand() % 2)
      out += '\n';
  }
  return out;
}

struct Result {
  std::string log;
  std::vector<u32> cursors;
  size_t fed;
};

Result RunWhole(const std::string &text, bool heuristics, unsigned budget) {
  g_log.clear();
  g_pages = 0;
  g_fed_since_page = 0;
  Book *book = reinterpret_cast<Book *>(&g_pages);
  static parsedata_t base;
  base.book = book;
  plain_text_stream::State state;
  plain_text_stream::InitState(&state, base, text, heuristics);
  Result r;
  unsigned guard = 0;
  while (!plain_text_stream::ContinueState(&state, text, 0, (u16)budget, 0,
                                           &r.cursors, NULL, Callbacks()))
    test::ExpectTrue("whole finishes", ++guard < 100000);
  r.log = g_log;
  r.fed = state.text_bytes_fed;
  return r;
}

Result RunWindowed(const std::string &text, bool heuristics, unsigned budget) {
  g_log.clear();
  g_pages = 0;
  g_fed_since_page = 0;
  Book *book = reinterpret_cast<Book *>(&g_pages);
  static parsedata_t base;
  base.book = book;
  plain_text_stream::State state;
  plain_text_stream::InitStateWindowed(&state, base, heuristics);
  std::string window;
  size_t at = 0;
  const unsigned chunk_mode = Rand() % 3;
  Result r;
  unsigned guard = 0;
  for (;;) {
    const bool done = plain_text_stream::ContinueState(
        &state, window, 0, (u16)budget, 0, &r.cursors, NULL, Callbacks());
    if (done)
      break;
    test::ExpectTrue("windowed makes progress", ++guard < 1000000);
    if (state.wants_data) {
      test::ExpectTrue("input remains", at < text.size() || !state.window_final);
      size_t n = chunk_mode == 0 ? 1 : chunk_mode == 1 ? 1 + Rand() % 20 : 1 + Rand() % 300;
      if (at + n > text.size())
        n = text.size() - at;
      const bool final = at + n == text.size();
      test::ExpectTrue("append", plain_text_stream::AppendWindow(
                                     &state, &window, text.data() + at, n, final));
      at += n;
    }
  }
  r.log = g_log;
  r.fed = state.text_bytes_fed;
  return r;
}
} // namespace

int main() {
  for (unsigned round = 0; round < 600; ++round) {
    const std::string text = RandomLines(Rand() % 60);
    const bool heuristics = Rand() % 2;
    const unsigned budget = Rand() % 3 ? 0 : 1 + Rand() % 3;
    const Result a = RunWhole(text, heuristics, budget);
    const Result b = RunWindowed(text, heuristics, budget);
    if (a.log != b.log || a.cursors != b.cursors || a.fed != b.fed) {
      fprintf(stderr, "text=[%s]\nwhole=[%s]\nwin  =[%s]\n", text.c_str(),
              a.log.c_str(), b.log.c_str());
    }
    test::ExpectTrue("same chardata sequence", a.log == b.log);
    test::ExpectTrue("same page cursors", a.cursors == b.cursors);
    test::ExpectEqU("same fed bytes", (unsigned)b.fed, (unsigned)a.fed);
  }
  return 0;
}
