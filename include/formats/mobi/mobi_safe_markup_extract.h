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

std::string ExtractToText(const std::string &markup_utf8,
                          const InlineImageCallbacks &image_callbacks =
                              InlineImageCallbacks(),
                          std::vector<std::pair<uint32_t, uint32_t>>
                              *html_to_text_map = nullptr);

} // namespace mobi_safe_markup_extract
