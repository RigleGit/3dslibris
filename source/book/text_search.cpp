#include "book/text_search.h"
#include "shared/text_token_constants.h"
#include "utf8proc.h"
#include <algorithm>
#include <cstring>

namespace text_search {
namespace {
struct Character {
  uint32_t cp;
  Position start, end;
  bool word_before;
  Character(uint32_t c = 0, Position s = {}, Position e = {})
      : cp(c), start(s), end(e), word_before(false) {}
};
bool Rtl(uint32_t cp) {
  const int bidi = utf8proc_get_property(cp)->bidi_class;
  return bidi == UTF8PROC_BIDI_CLASS_R || bidi == UTF8PROC_BIDI_CLASS_AL ||
         bidi == UTF8PROC_BIDI_CLASS_RLE || bidi == UTF8PROC_BIDI_CLASS_RLO ||
         bidi == UTF8PROC_BIDI_CLASS_RLI;
}
bool Mark(uint32_t cp) {
  int c = utf8proc_category(cp);
  return c == UTF8PROC_CATEGORY_MN || c == UTF8PROC_CATEGORY_MC || c == UTF8PROC_CATEGORY_ME;
}
bool Word(uint32_t cp) {
  int c = utf8proc_category(cp);
  return (c >= UTF8PROC_CATEGORY_LU && c <= UTF8PROC_CATEGORY_NO) || c == UTF8PROC_CATEGORY_PC;
}
bool Space(uint32_t cp) {
  int c = utf8proc_category(cp);
  return (cp >= 9 && cp <= 13) || cp == 0x85 ||
         c == UTF8PROC_CATEGORY_ZS || c == UTF8PROC_CATEGORY_ZL || c == UTF8PROC_CATEGORY_ZP;
}
int Decompose(uint32_t cp, int32_t *out) {
  return (int)utf8proc_decompose_char(cp, out, 32,
      (utf8proc_option_t)(UTF8PROC_DECOMPOSE | UTF8PROC_CASEFOLD | UTF8PROC_STABLE), NULL);
}
// Drawing tokens with arguments must be consumed atomically, never interpreted
// as letters. TEXT_LINE_PX aliases '#'; it is not emitted/handled by Page::Draw.
unsigned Payload(uint32_t cp) {
  switch (cp) {
  case TEXT_HR_BOUNDS: return 2;
  case TEXT_IMAGE: case TEXT_IMAGE_ALIGN: case TEXT_IMAGE_AUTHOR_WIDTH:
  case TEXT_LINE_START_X: case TEXT_FONT_SIZE: case TEXT_RTL_LINE_PX:
  case TEXT_LINK_START: case TEXT_UNDERLINE_STYLE: return 1;
  default: return 0;
  }
}
} // namespace

struct Engine::State {
  Source source{};
  Position anchor, cursor;
  Match match{}, pending{}, candidate{};
  Status status = Idle;
  std::string error;
  std::vector<uint32_t> query;
  std::vector<size_t> prefix;
  std::vector<Character> ring, cluster, ready;
  Character decomposition[32];
  size_t decomposition_count = 0, decomposition_index = 0, ready_index = 0;
  size_t matched = 0, ring_next = 0, ring_count = 0;
  unsigned unit = 0, tail = 0;
  uint64_t scanned = 0;
  bool whole = false, backwards = false, include = false, wrapped = false;
  bool excluded = false, annotated = false, raw_end = false, in_rtl = false;
  bool have_pending = false, have_candidate = false, previous_word = false, previous_space = false;

  void Fail(const char *message) { error = message; status = Failed; }
  void ResetUnit() {
    cursor = Position(unit ? unit - 1 : 0, 0);
    annotated = raw_end = in_rtl = false;
    matched = ring_next = ring_count = tail = 0;
    cluster.clear(); ready.clear(); ready_index = 0;
    decomposition_count = decomposition_index = 0;
    have_pending = have_candidate = previous_word = previous_space = false;
  }
  bool Eligible(Position p) const {
    if (p.page != unit) return false;
    if (p == anchor) return include && !wrapped;
    if (backwards) return wrapped ? anchor < p : p < anchor;
    return wrapped ? p < anchor : anchor < p;
  }
  void Accept(const Match &m) {
    if (!Eligible(m.start)) return;
    if (backwards) { candidate = m; have_candidate = true; }
    else { match = m; status = Found; }
  }
  void FinishUnit() {
    if (have_candidate) { match = candidate; status = Found; return; }
    if (wrapped && unit == anchor.page) { status = NotFound; return; }
    if (backwards) {
      if (unit == 0) { unit = source.pages - 1; wrapped = true; }
      else --unit;
    } else {
      if (++unit == source.pages) { unit = 0; wrapped = true; }
    }
    ResetUnit();
  }
  // One call handles at most one token or annotation. Payloads have fixed size.
  int Raw(Character *out) {
    if (cursor.page >= source.pages) return -1;
    PageView page{};
    if (!source.page(source.context, cursor.page, &page) ||
        (page.length && !page.text) || !page.events || cursor.token > page.length) {
      Fail("Invalid page data"); return -1;
    }
    unsigned flags = page_search::FlagsAt(*page.events, cursor.token);
    if (!annotated) {
      annotated = true;
      if (flags & page_search::RtlEnd) in_rtl = false;
      if (flags & page_search::RtlStart) { in_rtl = true; excluded = true; }
      if (flags & (page_search::Barrier | page_search::RtlStart | page_search::RtlEnd)) {
        if (flags & page_search::Skip) excluded = true;
        *out = Character(0, cursor, cursor); return 1;
      }
      if (flags & page_search::Space) { *out = Character(' ', cursor, cursor); return 1; }
    }
    annotated = false;
    if (cursor.token == page.length) { in_rtl = false; ++cursor.page; cursor.token = 0; return 0; }
    Position start = cursor;
    uint32_t cp = page.text[cursor.token++];
    ++scanned;
    unsigned payload = Payload(cp);
    if (payload > page.length - cursor.token) { Fail("Truncated page token"); return -1; }
    cursor.token += payload;
    if (in_rtl || (flags & page_search::Skip)) return 0;
    if (cp == TEXT_IMAGE || cp == TEXT_HR || cp == TEXT_HR_BOUNDS || Rtl(cp)) {
      if (Rtl(cp)) excluded = true;
      *out = Character(0, start, cursor); return 1;
    }
    if (cp == '\n' || cp == '\r' || cp == '\t' || cp == TEXT_PARAGRAPH_LEFT ||
        cp == TEXT_PARAGRAPH_CENTER || cp == TEXT_PARAGRAPH_RIGHT) {
      *out = Character(' ', start, cursor); return 1;
    }
    if (payload || cp < 32 || cp > 0x10ffff) return 0;
    *out = Character(cp, start, cursor);
    return 1;
  }
  void Feed(Character c) {
    if (Space(c.cp)) c.cp = ' ';
    if (have_pending) {
      // Canonical decomposition must not make "cafe" match half of "café".
      if (!Mark(c.cp) && (!whole || !Word(c.cp))) Accept(pending);
      have_pending = false;
      if (status != Searching) return;
    }
    if (c.cp == ' ' && previous_space) return;
    previous_space = c.cp == ' ';
    if (!c.cp) { matched = ring_count = ring_next = 0; previous_word = false; return; }
    c.word_before = previous_word;
    previous_word = Word(c.cp);
    ring[ring_next] = c;
    ring_next = (ring_next + 1) % ring.size();
    if (ring_count < ring.size()) ++ring_count;
    while (matched && query[matched] != c.cp) matched = prefix[matched - 1];
    if (query[matched] == c.cp) ++matched;
    if (matched == query.size()) {
      const Character &first = ring[(ring_next + ring.size() - query.size()) % ring.size()];
      if ((!whole || !first.word_before) && Eligible(first.start)) {
        pending.start = first.start; pending.end = c.end;
        // Canonical mark ordering can reorder source coordinates.
        for (size_t j = 0; j < query.size(); ++j) {
          const Character &part = ring[(ring_next + j) % ring.size()];
          if (part.start < pending.start) pending.start = part.start;
          if (pending.end < part.end) pending.end = part.end;
        }
        have_pending = true;
      }
      matched = prefix[matched - 1];
    }
  }
  void Step() {
    if (ready_index < ready.size()) {
      Character c = ready[ready_index++];
      const bool collapsed_space = Space(c.cp) && previous_space;
      Feed(c);
      if (status != Searching) return;
      if (!collapsed_space && c.start.page > unit && ++tail > query.size() + 2) FinishUnit();
      return;
    }
    ready.clear(); ready_index = 0;
    if (decomposition_index < decomposition_count) {
      Character c = decomposition[decomposition_index];
      const int combining = utf8proc_get_property(c.cp)->combining_class;
      if ((!combining || !c.cp) && !cluster.empty()) {
        ready.swap(cluster); return;
      }
      ++decomposition_index;
      if (cluster.size() >= 256) { Fail("Text combining sequence too long"); return; }
      size_t pos = cluster.size();
      cluster.push_back(c);
      // Stable canonical ordering, with source coordinates retained.
      while (combining && pos && utf8proc_get_property(cluster[pos-1].cp)->combining_class > combining) {
        cluster[pos] = cluster[pos-1]; --pos;
      }
      cluster[pos] = c;
      if (!c.cp) ready.swap(cluster);
      return;
    }
    if (raw_end) {
      if (!cluster.empty()) { ready.swap(cluster); return; }
      Feed(Character());
      if (status == Searching) FinishUnit();
      return;
    }
    Character c;
    int got = Raw(&c);
    if (status != Searching) return;
    if (got < 0) { raw_end = true; return; }
    if (!got) return;
    int32_t cps[32];
    int count = c.cp ? Decompose(c.cp, cps) : 1;
    if (!c.cp) cps[0] = 0;
    if (count < 0 || count > 32) { Fail("Invalid Unicode text"); return; }
    decomposition_index = 0; decomposition_count = count;
    for (int i = 0; i < count; ++i) decomposition[i] = Character(cps[i], c.start, c.end);
  }
};

Engine::Engine() : state_(new State) {}
Engine::~Engine() {}
bool Engine::Configure(const std::string &query, bool whole_words) {
  State &s = *state_;
  s.status = Idle; s.error.clear(); s.query.clear(); s.whole = whole_words;
  size_t offset = 0, units = 0;
  while (offset < query.size()) {
    int32_t cp;
    auto n = utf8proc_iterate((const uint8_t *)query.data() + offset, query.size() - offset, &cp);
    if (n <= 0 || cp == 0) { s.Fail("Invalid search text"); return false; }
    offset += n; units += cp > 0xffff ? 2 : 1;
    if (units > 128) { s.Fail("Search is limited to 128 characters"); return false; }
    if (Rtl(cp)) { s.Fail("RTL search is not supported yet"); return false; }
  }
  uint8_t *normalized = NULL;
  auto count = utf8proc_map((const uint8_t *)query.data(), query.size(), &normalized,
      (utf8proc_option_t)(UTF8PROC_DECOMPOSE | UTF8PROC_CASEFOLD | UTF8PROC_STABLE));
  if (count < 0) { s.Fail("Cannot normalize search text"); return false; }
  offset = 0;
  while (offset < (size_t)count) {
    int32_t cp;
    auto n = utf8proc_iterate(normalized + offset, count - offset, &cp);
    offset += n;
    if (Space(cp)) {
      if (!s.query.empty() && s.query.back() != ' ') s.query.push_back(' ');
    } else s.query.push_back(cp);
  }
  free(normalized);
  if (!s.query.empty() && s.query.back() == ' ') s.query.pop_back();
  if (s.query.empty()) { s.Fail("Enter a word or phrase"); return false; }
  s.prefix.assign(s.query.size(), 0);
  for (size_t i = 1, k = 0; i < s.query.size(); ++i) {
    while (k && s.query[i] != s.query[k]) k = s.prefix[k-1];
    if (s.query[i] == s.query[k]) ++k;
    s.prefix[i] = k;
  }
  s.ring.resize(s.query.size());
  s.cluster.reserve(256); s.ready.reserve(256);
  return true;
}
void Engine::Start(const Source &source, Position anchor, bool backwards, bool include_anchor) {
  State &s = *state_;
  if (s.query.empty() || !source.pages || !source.page || anchor.page >= source.pages) {
    s.Fail("No searchable pages"); return;
  }
  s.source = source; s.anchor = anchor; s.unit = anchor.page;
  s.backwards = backwards; s.include = include_anchor; s.wrapped = s.excluded = false;
  s.scanned = 0; s.status = Searching; s.error.clear(); s.ResetUnit();
}
Status Engine::Pump(unsigned steps) {
  while (steps-- && state_->status == Searching) state_->Step();
  return state_->status;
}
void Engine::Cancel() { state_->status = Idle; state_->source = Source{}; }
Status Engine::GetStatus() const { return state_->status; }
const Match &Engine::GetMatch() const { return state_->match; }
const std::string &Engine::GetError() const { return state_->error; }
bool Engine::Wrapped() const { return state_->wrapped; }
bool Engine::ExcludedRtl() const { return state_->excluded; }
uint64_t Engine::Scanned() const { return state_->scanned; }
size_t Engine::AuxiliaryBytes() const {
  const State &s = *state_;
  return sizeof(State) + s.query.capacity()*sizeof(uint32_t) + s.prefix.capacity()*sizeof(size_t) +
      (s.ring.capacity()+s.cluster.capacity()+s.ready.capacity())*sizeof(Character);
}
} // namespace text_search
