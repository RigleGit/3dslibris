#include "formats/mobi/mobi_position_map.h"

#include <stddef.h>

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

void RemapHtmlToTextAfterCleanup(
    const std::string &text_before_cleanup, const std::string &text_after_cleanup,
    std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map) {
  if (!html_to_text_map || html_to_text_map->empty())
    return;

  const size_t pre_len = text_before_cleanup.size();
  const size_t post_len = text_after_cleanup.size();
  std::vector<std::pair<uint32_t, uint32_t>> original = *html_to_text_map;

  size_t pi = 0, qi = 0, entry = 0;
  const size_t n_entries = html_to_text_map->size();
  while (entry < n_entries && pi <= pre_len && qi <= post_len) {
    const uint32_t target = (*html_to_text_map)[entry].second;
    if ((size_t)target <= pi) {
      (*html_to_text_map)[entry].second =
          (uint32_t)(qi - (pi - (size_t)target));
      entry++;
      continue;
    }
    while (pi < (size_t)target && pi < pre_len && qi < post_len) {
      if (text_before_cleanup[pi] == text_after_cleanup[qi]) {
        pi++;
        qi++;
      } else {
        pi++;
      }
    }
  }
  while (entry < n_entries) {
    (*html_to_text_map)[entry].second = (uint32_t)post_len;
    entry++;
  }

  if (!LooksPlausible(*html_to_text_map, post_len)) {
    *html_to_text_map = original;
    ScaleHtmlToTextMap(pre_len, post_len, html_to_text_map);
  }
}

} // namespace mobi_position_map
