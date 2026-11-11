#ifndef MOBI_POSITION_MAP_H
#define MOBI_POSITION_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace mobi_position_map {

size_t HtmlSampleIntervalForTextBytes(size_t text_bytes);

bool LooksUsableForToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page);

void RemapHtmlToTextAfterCleanup(
    const std::string &text_before_cleanup,
    const std::string &text_after_cleanup,
    std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map);

// Same remap without holding either text, resumable: byte accessors are called
// with non-decreasing indexes (an index may repeat). The map is final once Step
// returns true; an empty map is left alone.
typedef unsigned char (*ByteAtFn)(void *context, size_t index);
class StreamRemap {
public:
  StreamRemap();
  ~StreamRemap();
  bool Begin(size_t pre_len, size_t post_len, ByteAtFn before_at,
             void *before_context, ByteAtFn after_at, void *after_context,
             std::vector<std::pair<uint32_t, uint32_t>> *html_to_text_map);
  // Compares at most `budget` bytes; true when finished.
  bool Step(size_t budget);
  size_t Progress() const;
private:
  struct State;
  State *state_;
  StreamRemap(const StreamRemap &);
  StreamRemap &operator=(const StreamRemap &);
};

} // namespace mobi_position_map

#endif
