/*
    3dslibris - mobi_toc_apply.cpp

    Apply structured MOBI TOC entries to chapter pages.
*/

#include "formats/mobi/mobi_toc_apply.h"

#include <ctype.h>
#include <set>
#include <new>
#include <stdio.h>

#include "book/book.h"
#include "shared/debug_log.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/mobi/mobi_position_map.h"
#include "shared/status_reporter.h"

namespace {

static const u32 kMobiNullIndex = mobi_structured_toc_parser::kMobiNullIndex;
typedef mobi_structured_toc_parser::MobiStructuredTocEntry
    MobiStructuredTocEntry;

std::string TrimAsciiWhitespaceLocal(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

std::string CollapseAsciiWhitespaceLocal(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool last_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isspace(c)) {
      if (!last_space && !out.empty()) {
        out.push_back(' ');
        last_space = true;
      }
      continue;
    }
    out.push_back((char)c);
    last_space = false;
  }
  return out;
}

} // namespace

namespace mobi_toc_apply {

int HtmlPosToPage(u32 html_pos,
                  const std::vector<std::pair<u32, u32>> &html_to_text_map,
                  const std::vector<u32> &text_cursor_per_page,
                  u32 *text_pos_out) {
  if (text_pos_out)
    *text_pos_out = 0;
  if (html_to_text_map.size() < 2 || text_cursor_per_page.empty())
    return -1;

  size_t lo = 0, hi = html_to_text_map.size() - 1;
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if (html_to_text_map[mid].first <= html_pos)
      lo = mid;
    else
      hi = mid;
  }
  const u32 h0 = html_to_text_map[lo].first;
  const u32 t0 = html_to_text_map[lo].second;
  const u32 h1 = html_to_text_map[hi].first;
  const u32 t1 = html_to_text_map[hi].second;
  u32 text_pos;
  if (h1 == h0)
    text_pos = t0;
  else {
    double frac = (double)(html_pos - h0) / (double)(h1 - h0);
    if (frac < 0.0)
      frac = 0.0;
    if (frac > 1.0)
      frac = 1.0;
    text_pos = t0 + (u32)(frac * (double)(t1 - t0));
  }

  if (text_pos_out)
    *text_pos_out = text_pos;

  lo = 0;
  hi = text_cursor_per_page.size();
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if (text_cursor_per_page[mid] <= text_pos)
      lo = mid;
    else
      hi = mid;
  }
  return (int)lo;
}

struct Session::State {
  Book *book;
  const std::vector<MobiStructuredTocEntry> *entries;
  const std::vector<std::pair<u32, u32>> *map;
  const std::vector<u32> *cursors;
  BuildCallbacks callbacks;
  u32 text_len;
  unsigned count, entry = 0, scan_start = 0, probe = 0;
  size_t mapped = 0, direct = 0;
  bool precise, search, has_pos = false;
  enum Phase { Entry, Near, Scan, Commit, Done } phase = Entry;
  int best = -1;
  std::string clean, needle;
  std::set<u16> used;
  bool Matches(unsigned page) {
    return callbacks.page_has_heading_needle &&
           callbacks.page_has_heading_needle(
               page_text_extract_utils::ExtractTextLinesFromPage(
                   book->GetPage(page)),
               needle);
  }
};
Session::Session() : state_(NULL) {}
Session::~Session() {
  delete state_;
}
bool Session::Begin(Book *book,
                    const std::vector<MobiStructuredTocEntry> *entries,
                    u32 text_len, bool refine,
                    const std::vector<std::pair<u32, u32>> *map,
                    const std::vector<u32> *cursors,
                    const BuildCallbacks &callbacks) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !book || !entries || !map || !cursors)
    return false;
  State &s = *state_;
  s.book = book;
  s.entries = entries;
  s.text_len = text_len;
  s.map = map;
  s.cursors = cursors;
  s.callbacks = callbacks;
  s.count = book->GetPageCount();
  s.precise = mobi_position_map::LooksUsableForToc(*map, *cursors);
  s.search = refine || s.precise;
  for (const auto &e : *entries)
    if (e.pos == kMobiNullIndex)
      s.search = true;
  if (entries->empty() || !s.count)
    s.phase = State::Done;
  return true;
}
size_t Session::Mapped() const {
  return state_ ? state_->mapped : 0;
}
size_t Session::Direct() const {
  return state_ ? state_->direct : 0;
}
unsigned Session::Progress() const {
  return state_ ? state_->entry : 0;
}
bool Session::Step() {
  if (!state_ || state_->phase == State::Done)
    return true;
  State &s = *state_;
  if (s.phase == State::Entry) {
    if (s.entry == s.entries->size()) {
      s.phase = State::Done;
      return true;
    }
    const auto &e = (*s.entries)[s.entry];
    s.clean = CollapseAsciiWhitespaceLocal(TrimAsciiWhitespaceLocal(e.title));
    if (s.clean.empty()) {
      ++s.entry;
      return false;
    }
    s.needle = s.callbacks.normalize_heading_needle
                   ? s.callbacks.normalize_heading_needle(s.clean)
                   : s.clean;
    s.has_pos = e.pos != kMobiNullIndex;
    s.best = -1;
    s.probe = 0;
    if (s.has_pos) {
      if (s.precise)
        s.best = HtmlPosToPage(e.pos, *s.map, *s.cursors, NULL);
      if (s.best < 0 || s.best >= (int)s.count) {
        double ratio = (double)e.pos / (double)(s.text_len ? s.text_len : 1);
        if (ratio > 1)
          ratio = 1;
        s.best = (u16)(ratio * (double)(s.count - 1));
      }
      s.phase = s.search && s.callbacks.page_has_heading_needle ? State::Near
                                                                : State::Commit;
    } else
      s.phase = s.search && s.callbacks.page_has_heading_needle ? State::Scan
                                                                : State::Commit;
  } else if (s.phase == State::Near) {
    // Exactly the previous order: guess, guess-1, guess+1, ... radius four.
    const int delta = (s.probe + 1) / 2;
    const int page = s.best + (s.probe % 2 ? -delta : delta);
    ++s.probe;
    if (page >= 0 && page < (int)s.count && s.needle.size() >= 3 &&
        s.Matches(page)) {
      s.best = page;
      s.phase = State::Commit;
    } else if (s.probe == 9)
      s.phase = State::Commit;
  } else if (s.phase == State::Scan) {
    if (s.probe == s.count)
      s.phase = State::Commit;
    else {
      const unsigned p = (s.scan_start + s.probe++) % s.count;
      if (s.Matches(p)) {
        s.best = p;
        s.phase = State::Commit;
      }
    }
  } else if (s.phase == State::Commit) {
    if (s.best >= 0 && s.best < (int)s.count && s.used.insert(s.best).second) {
      const auto &e = (*s.entries)[s.entry];
      if (s.callbacks.add_chapter_at_page_if_unique)
        s.callbacks.add_chapter_at_page_if_unique(s.book, s.best, s.clean,
                                                  e.level);
      else
        s.book->AddChapter(s.best, s.clean, e.level);
      ++s.mapped;
      if (s.has_pos)
        ++s.direct;
      s.scan_start = s.best;
    }
    ++s.entry;
    s.phase = State::Entry;
  }
  return false;
}
size_t BuildChaptersFromStructuredToc(
    Book *book, const std::vector<MobiStructuredTocEntry> &entries,
    u32 text_len, size_t *direct_out, bool refine,
    const std::vector<std::pair<u32, u32>> &map,
    const std::vector<u32> &cursors, const BuildCallbacks &callbacks,
    IStatusReporter *reporter) {
  Session session;
  if (!session.Begin(book, &entries, text_len, refine, &map, &cursors,
                     callbacks)) {
    if (direct_out)
      *direct_out = 0;
    return 0;
  }
  while (!session.Step()) {
  }
  if (direct_out)
    *direct_out = session.Direct();
  DBG_LOGF(reporter, "MOBI TOC mapped=%u direct=%u", (unsigned)session.Mapped(),
           (unsigned)session.Direct());
  return session.Mapped();
}

} // namespace mobi_toc_apply
