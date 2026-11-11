#include "formats/mobi/mobi_position_map.h"

#include <stddef.h>
#include <new>

namespace mobi_position_map {

namespace {

static void ScaleHtmlToTextMap(
    size_t pre_len, size_t post_len,
    std::vector<std::pair<uint32_t, uint32_t>> *map) {
  if (!map)
    return;
  if (pre_len == 0) {
    for (size_t i = 0; i < map->size(); i++)
      (*map)[i].second = (uint32_t)post_len;
    return;
  }

  uint32_t prev = 0;
  for (size_t i = 0; i < map->size(); i++) {
    const size_t target = (size_t)(*map)[i].second;
    size_t scaled = (target * post_len) / pre_len;
    if (scaled < prev)
      scaled = prev;
    if (scaled > post_len)
      scaled = post_len;
    (*map)[i].second = (uint32_t)scaled;
    prev = (uint32_t)scaled;
  }
  if (!map->empty())
    map->back().second = (uint32_t)post_len;
}

static bool LooksPlausible(
    const std::vector<std::pair<uint32_t, uint32_t>> &map, size_t post_len) {
  if (map.empty())
    return true;
  if ((size_t)map.back().second != post_len)
    return false;
  uint32_t prev = 0;
  for (size_t i = 0; i < map.size(); i++) {
    if (map[i].second < prev)
      return false;
    prev = map[i].second;
  }
  if (post_len >= 4096 && (size_t)map.back().second < post_len / 2)
    return false;
  return true;
}

} // namespace

size_t HtmlSampleIntervalForTextBytes(size_t text_bytes) {
  return (text_bytes >= 1024 * 1024) ? 512u : 256u;
}

bool LooksUsableForToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page) {
  if (html_to_text_map.size() < 2 || text_cursor_per_page.empty())
    return false;

  const uint32_t map_last_text = html_to_text_map.back().second;
  const uint32_t pages_last_cursor = text_cursor_per_page.back();
  if (map_last_text < pages_last_cursor)
    return false;
  if (pages_last_cursor >= 4096 && map_last_text < pages_last_cursor / 2)
    return false;
  return true;
}

namespace {

struct RemapProgress {
  size_t pi, qi, entry;
  RemapProgress() : pi(0), qi(0), entry(0) {}
};

// The remap walks both texts forward only, so they can be read sequentially
// and the walk can be resumed: it stops after `budget` compared bytes.
template <class BeforeAt, class AfterAt>
bool RemapAdvance(RemapProgress *p, size_t pre_len, size_t post_len,
                  BeforeAt before, AfterAt after, size_t budget,
                  std::vector<std::pair<uint32_t, uint32_t>> *map) {
  size_t &pi = p->pi, &qi = p->qi, &entry = p->entry;
  const size_t n_entries = map->size();
  while (entry < n_entries && pi <= pre_len && qi <= post_len) {
    const uint32_t target = (*map)[entry].second;
    if ((size_t)target <= pi) {
      (*map)[entry].second = (uint32_t)(qi - (pi - (size_t)target));
      entry++;
      continue;
    }
    while (pi < (size_t)target && pi < pre_len && qi < post_len) {
      if (!budget)
        return false;
      --budget;
      if (before(pi) == after(qi)) {
        pi++;
        qi++;
      } else {
        pi++;
      }
    }
    // The cleaned text ended before this target (trailing text was removed):
    // the remaining entries map to its end. Without this the walk never ends.
    if (pi < (size_t)target)
      break;
  }
  return true;
}

void RemapFinish(
    RemapProgress *p, size_t pre_len, size_t post_len,
    const std::vector<std::pair<uint32_t, uint32_t>> &original,
    std::vector<std::pair<uint32_t, uint32_t>> *map) {
  while (p->entry < map->size()) {
    (*map)[p->entry].second = (uint32_t)post_len;
    p->entry++;
  }
  if (!LooksPlausible(*map, post_len)) {
    *map = original;
    ScaleHtmlToTextMap(pre_len, post_len, map);
  }
}

struct StringByte {
  const std::string *s;
  unsigned char operator()(size_t i) const { return (unsigned char)(*s)[i]; }
};
struct CallbackByte {
  ByteAtFn fn;
  void *context;
  unsigned char operator()(size_t i) const { return fn(context, i); }
};

} // namespace

void RemapHtmlToTextAfterCleanup(
    const std::string &text_before_cleanup, const std::string &text_after_cleanup,
    std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map) {
  if (!html_to_text_map || html_to_text_map->empty())
    return;
  const std::vector<std::pair<uint32_t, uint32_t>> original = *html_to_text_map;
  StringByte before = {&text_before_cleanup}, after = {&text_after_cleanup};
  RemapProgress progress;
  RemapAdvance(&progress, before.s->size(), after.s->size(), before, after,
               (size_t)-1, html_to_text_map);
  RemapFinish(&progress, before.s->size(), after.s->size(), original,
              html_to_text_map);
}

struct StreamRemap::State {
  size_t pre_len, post_len;
  CallbackByte before, after;
  std::vector<std::pair<uint32_t, uint32_t>> *map;
  std::vector<std::pair<uint32_t, uint32_t>> original;
  RemapProgress progress;
};
StreamRemap::StreamRemap() : state_(NULL) {}
StreamRemap::~StreamRemap() { delete state_; }
bool StreamRemap::Begin(
    size_t pre_len, size_t post_len, ByteAtFn before_at, void *before_context,
    ByteAtFn after_at, void *after_context,
    std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map) {
  delete state_;
  state_ = NULL;
  if (!html_to_text_map || !before_at || !after_at)
    return false;
  state_ = new (std::nothrow) State;
  if (!state_)
    return false;
  state_->pre_len = pre_len;
  state_->post_len = post_len;
  state_->before.fn = before_at;
  state_->before.context = before_context;
  state_->after.fn = after_at;
  state_->after.context = after_context;
  state_->map = html_to_text_map;
  state_->original = *html_to_text_map;
  return true;
}
bool StreamRemap::Step(size_t budget) {
  if (!state_)
    return true;
  State &s = *state_;
  if (!RemapAdvance(&s.progress, s.pre_len, s.post_len, s.before, s.after,
                    budget, s.map))
    return false;
  RemapFinish(&s.progress, s.pre_len, s.post_len, s.original, s.map);
  delete state_;
  state_ = NULL;
  return true;
}
size_t StreamRemap::Progress() const {
  return state_ ? state_->progress.pi : 0;
}

} // namespace mobi_position_map
