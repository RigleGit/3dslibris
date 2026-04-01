#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace rtf_control_word_utils {

using CodepointAppender = void (*)(std::string *out, uint32_t cp);

inline bool EqualsLiteral(const char *word, size_t len, const char *literal) {
  const size_t literal_len = std::strlen(literal);
  return len == literal_len && std::memcmp(word, literal, len) == 0;
}

inline bool AppendReplacement(const char *word, size_t len, std::string *out,
                              CodepointAppender append_codepoint) {
  if (!word || !out || !append_codepoint)
    return false;

  if (EqualsLiteral(word, len, "par") || EqualsLiteral(word, len, "line")) {
    out->push_back('\n');
    return true;
  }
  if (EqualsLiteral(word, len, "tab")) {
    out->push_back('\t');
    return true;
  }
  if (EqualsLiteral(word, len, "emdash")) {
    append_codepoint(out, 0x2014);
    return true;
  }
  if (EqualsLiteral(word, len, "endash")) {
    append_codepoint(out, 0x2013);
    return true;
  }
  if (EqualsLiteral(word, len, "bullet")) {
    append_codepoint(out, 0x2022);
    return true;
  }
  if (EqualsLiteral(word, len, "lquote")) {
    append_codepoint(out, 0x2018);
    return true;
  }
  if (EqualsLiteral(word, len, "rquote")) {
    append_codepoint(out, 0x2019);
    return true;
  }
  if (EqualsLiteral(word, len, "ldblquote")) {
    append_codepoint(out, 0x201C);
    return true;
  }
  if (EqualsLiteral(word, len, "rdblquote")) {
    append_codepoint(out, 0x201D);
    return true;
  }
  return false;
}

} // namespace rtf_control_word_utils
