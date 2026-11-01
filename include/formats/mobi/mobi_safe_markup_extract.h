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
private:
  struct State;
  State *state_;
  Session(const Session &);
  Session &operator=(const Session &);
};

std::string ExtractToText(const std::string &markup_utf8,
                          const InlineImageCallbacks &image_callbacks =
                              InlineImageCallbacks(),
                          std::vector<std::pair<uint32_t, uint32_t>>
                              *html_to_text_map = nullptr);

} // namespace mobi_safe_markup_extract
