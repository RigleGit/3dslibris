#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace text_unicode_utils {

struct TextCodepoint {
  uint32_t codepoint;
  size_t byte_offset;
  size_t byte_length;
  bool grapheme_start;
  bool whitespace;
  bool breakable_space;
  bool allow_break_after;
  bool must_break_after;
};

size_t DecodeNextDisplayCodepoint(const char *s, size_t len, uint32_t *out);
bool BuildTextRunUtf8(const char *s, size_t len, const char *lang,
                      std::vector<TextCodepoint> *out);
size_t Utf8BytesForDisplayChars(const char *s, size_t display_chars);
size_t StripLeadingListMarkerUtf8(const char *s);
size_t StripLeadingListMarkerUtf8(const char *s, size_t len,
                                  bool *all_whitespace_only);

} // namespace text_unicode_utils
