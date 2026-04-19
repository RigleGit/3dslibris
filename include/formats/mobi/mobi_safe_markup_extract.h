#pragma once

#include <stdint.h>

#include <string>

namespace mobi_safe_markup_extract {

struct InlineImageCallbacks {
  uint16_t (*register_inline_image)(void *user_data, const std::string &path);
  void *user_data;

  InlineImageCallbacks() : register_inline_image(nullptr), user_data(nullptr) {}
};

std::string ExtractToText(const std::string &markup_utf8,
                          const InlineImageCallbacks &image_callbacks =
                              InlineImageCallbacks());

} // namespace mobi_safe_markup_extract
