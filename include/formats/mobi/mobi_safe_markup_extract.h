#pragma once

#include <stdint.h>
#include <utility>
#include <vector>

#include <string>

namespace mobi_safe_markup_extract {

struct InlineImageCallbacks {
  uint16_t (*register_inline_image)(void *user_data, const std::string &path);
  void *user_data;

  InlineImageCallbacks() : register_inline_image(nullptr), user_data(nullptr) {}
};

class Session {
public:
  Session();
  ~Session();
  bool Begin(const std::string *source, std::string *output,
             const InlineImageCallbacks &callbacks,
             std::vector<std::pair<uint32_t, uint32_t>> *map = nullptr);
  bool Step(size_t bytes = 32768);
  size_t Progress() const;
  bool NoMemory() const;
private:
  struct State;
  State *state_;
  Session(const Session &);
  Session &operator=(const Session &);
};

// Incremental form of Session: markup arrives in pieces of any size and the
// text leaves in drained chunks, so neither side is resident whole. Output and
// html-to-text map equal the whole-buffer result for any split of the input.
class Stream {
public:
  Stream();
  ~Stream();
  // total_markup_bytes sets the map sampling interval (as the whole input's
  // size does for Session).
  bool Begin(const InlineImageCallbacks &callbacks,
             std::vector<std::pair<uint32_t, uint32_t>> *map,
             size_t total_markup_bytes);
  // The last piece must set `final`. False on out of memory.
  bool Feed(const char *data, size_t n, bool final);
  // Appends the output that can no longer change to *chunk; everything after
  // the final feed.
  void Drain(std::string *chunk);
  size_t Consumed() const;
  size_t OutputBytes() const;
  bool NoMemory() const;
private:
  struct State;
  State *state_;
  Stream(const Stream &);
  Stream &operator=(const Stream &);
};

std::string ExtractToText(const std::string &markup_utf8,
                          const InlineImageCallbacks &image_callbacks =
                              InlineImageCallbacks(),
                          std::vector<std::pair<uint32_t, uint32_t>>
                              *html_to_text_map = nullptr);

} // namespace mobi_safe_markup_extract
