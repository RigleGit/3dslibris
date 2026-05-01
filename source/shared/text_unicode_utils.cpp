#include "shared/text_unicode_utils.h"

#include <cstring>
#include <string>

extern "C" {
#include "utf8proc.h"
#include "linebreak.h"
}

namespace text_unicode_utils {
namespace {

uint32_t DecodeCp1252Byte(unsigned char b) {
  static const uint16_t kCp1252Map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  if (b < 0x80)
    return b;
  if (b <= 0x9F) {
    uint16_t mapped = kCp1252Map[b - 0x80];
    return mapped ? mapped : (uint32_t)'?';
  }
  return b;
}

size_t EncodeUtf8(uint32_t cp, char out[4]) {
  utf8proc_ssize_t n =
      utf8proc_encode_char((utf8proc_int32_t)cp, (utf8proc_uint8_t *)out);
  return (n > 0) ? (size_t)n : 0;
}

bool IsUnicodeWhitespaceCodepoint(uint32_t cp) {
  switch (cp) {
  case '\t':
  case '\n':
  case '\r':
  case 0x000B:
  case 0x000C:
  case 0x0085:
  case 0x2028:
  case 0x2029:
    return true;
  default:
    break;
  }

  utf8proc_category_t cat = utf8proc_category((utf8proc_int32_t)cp);
  return cat == UTF8PROC_CATEGORY_ZS || cat == UTF8PROC_CATEGORY_ZL ||
         cat == UTF8PROC_CATEGORY_ZP;
}

bool IsBreakableSpaceCodepoint(uint32_t cp) {
  if (!IsUnicodeWhitespaceCodepoint(cp))
    return false;
  return cp != 0x00A0 && cp != 0x202F && cp != 0x2060;
}

bool IsLikelyListMarkerCodepoint(uint32_t cp) {
  switch (cp) {
  case 0x00B7:
  case 0x2022:
  case 0x2023:
  case 0x2043:
  case 0x2219:
  case 0x25AA:
  case 0x25CF:
  case 0x25E6:
    return true;
  default:
    break;
  }

  return cp >= 0xF000 && cp <= 0xF8FF;
}

bool IsSimpleWhitespaceCodepoint(uint32_t cp) {
  switch (cp) {
  case '\t':
  case '\n':
  case '\r':
  case 0x000B:
  case 0x000C:
  case 0x0085:
  case 0x00A0:
  case 0x2028:
  case 0x2029:
  case 0x202F:
    return true;
  default:
    return cp == ' ';
  }
}

bool IsSimpleLtrCodepoint(uint32_t cp) {
  if (IsSimpleWhitespaceCodepoint(cp))
    return true;
  if (cp < 0x20 || cp == 0x7F)
    return false;
  if (cp < 0x0300)
    return true;
  if (cp >= 0x0300 && cp <= 0x036F)
    return false;
  if (cp < 0x0590)
    return true;
  if (cp >= 0x2010 && cp <= 0x203A)
    return true;
  return cp == 0x20AC || cp == 0x2122;
}

bool IsUtf8Continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

size_t DecodeNextSimpleDisplayCodepoint(const char *s, size_t len,
                                        uint32_t *out) {
  if (!s || !len || !out)
    return 0;

  const unsigned char b0 = (unsigned char)s[0];
  if (b0 < 0x80) {
    *out = b0;
    return 1;
  }

  if (b0 >= 0xC2 && b0 <= 0xDF && len >= 2) {
    const unsigned char b1 = (unsigned char)s[1];
    if (IsUtf8Continuation(b1)) {
      *out = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
      return 2;
    }
  } else if (b0 >= 0xE0 && b0 <= 0xEF && len >= 3) {
    const unsigned char b1 = (unsigned char)s[1];
    const unsigned char b2 = (unsigned char)s[2];
    const bool valid_min = b0 != 0xE0 || b1 >= 0xA0;
    const bool valid_surrogate = b0 != 0xED || b1 < 0xA0;
    if (valid_min && valid_surrogate && IsUtf8Continuation(b1) &&
        IsUtf8Continuation(b2)) {
      *out = ((uint32_t)(b0 & 0x0F) << 12) |
             ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
      return 3;
    }
  } else if (b0 >= 0xF0 && b0 <= 0xF4 && len >= 4) {
    const unsigned char b1 = (unsigned char)s[1];
    const unsigned char b2 = (unsigned char)s[2];
    const unsigned char b3 = (unsigned char)s[3];
    const bool valid_min = b0 != 0xF0 || b1 >= 0x90;
    const bool valid_max = b0 != 0xF4 || b1 <= 0x8F;
    if (valid_min && valid_max && IsUtf8Continuation(b1) &&
        IsUtf8Continuation(b2) && IsUtf8Continuation(b3)) {
      *out = ((uint32_t)(b0 & 0x07) << 18) |
             ((uint32_t)(b1 & 0x3F) << 12) |
             ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
      return 4;
    }
  }

  *out = DecodeCp1252Byte(b0);
  return 1;
}

bool AllowsSimpleBreakAfter(uint32_t cp) {
  return cp == '-' || cp == '/' || cp == 0x2010 || cp == 0x2013 ||
         cp == 0x2014;
}

bool BuildTextRunUtf8Simple(const char *s, size_t len,
                            std::vector<TextCodepoint> *out) {
  if (!s || !out)
    return false;

  out->clear();
  size_t offset = 0;
  while (offset < len && s[offset] != '\0') {
    uint32_t cp = 0;
    size_t step =
        DecodeNextSimpleDisplayCodepoint(s + offset, len - offset, &cp);
    if (!step)
      return false;
    if (!IsSimpleLtrCodepoint(cp))
      return false;

    TextCodepoint unit;
    unit.codepoint = cp;
    unit.byte_offset = offset;
    unit.byte_length = step;
    unit.grapheme_start = true;
    unit.whitespace = IsSimpleWhitespaceCodepoint(cp);
    unit.breakable_space =
        unit.whitespace && cp != 0x00A0 && cp != 0x202F && cp != 0x2060;
    unit.allow_break_after = unit.breakable_space || AllowsSimpleBreakAfter(cp);
    unit.must_break_after =
        cp == '\n' || cp == '\r' || cp == 0x000B || cp == 0x000C ||
        cp == 0x0085 || cp == 0x2028 || cp == 0x2029;
    out->push_back(unit);
    offset += step;
  }
  return true;
}

size_t ConsumeOrderedListMarkerUtf8(const char *s, size_t len) {
  if (!s || !len)
    return 0;

  size_t offset = 0;
  bool saw_open_paren = false;
  if (s[offset] == '(') {
    saw_open_paren = true;
    offset++;
  }

  const size_t digits_start = offset;
  while (offset < len && s[offset] >= '0' && s[offset] <= '9')
    offset++;
  if (offset == digits_start)
    return 0;

  if (saw_open_paren) {
    if (offset >= len || s[offset] != ')')
      return 0;
    offset++;
  } else {
    if (offset >= len || (s[offset] != '.' && s[offset] != ')'))
      return 0;
    offset++;
  }

  size_t ws_start = offset;
  while (offset < len && s[offset] != '\0') {
    uint32_t cp = 0;
    size_t step = DecodeNextDisplayCodepoint(s + offset, len - offset, &cp);
    if (!step || !IsUnicodeWhitespaceCodepoint(cp))
      break;
    offset += step;
  }

  return (offset > ws_start) ? offset : 0;
}

} // namespace

size_t DecodeNextDisplayCodepoint(const char *s, size_t len, uint32_t *out) {
  if (!s || !len || !out)
    return 0;

  utf8proc_int32_t cp = -1;
  utf8proc_ssize_t step =
      utf8proc_iterate((const utf8proc_uint8_t *)s, (utf8proc_ssize_t)len, &cp);
  if (step > 0 && cp >= 0) {
    *out = (uint32_t)cp;
    return (size_t)step;
  }

  *out = DecodeCp1252Byte((unsigned char)s[0]);
  return 1;
}

bool BuildTextRunUtf8(const char *s, size_t len, const char *lang,
                      std::vector<TextCodepoint> *out) {
  if (!s || !out)
    return false;

  if (BuildTextRunUtf8Simple(s, len, out))
    return true;

  out->clear();
  static std::string normalized;
  normalized.clear();
  if (normalized.capacity() < len * 2)
    normalized.reserve(len * 2);

  size_t offset = 0;
  uint32_t prev_cp = 0;
  utf8proc_int32_t grapheme_state = 0;
  while (offset < len && s[offset] != '\0') {
    uint32_t cp = 0;
    size_t step = DecodeNextDisplayCodepoint(s + offset, len - offset, &cp);
    if (!step)
      return false;

    TextCodepoint unit;
    unit.codepoint = cp;
    unit.byte_offset = offset;
    unit.byte_length = step;
    unit.grapheme_start =
        out->empty() ||
        utf8proc_grapheme_break_stateful((utf8proc_int32_t)prev_cp,
                                         (utf8proc_int32_t)cp,
                                         &grapheme_state) != 0;
    if (unit.grapheme_start)
      grapheme_state = 0;
    unit.whitespace = IsUnicodeWhitespaceCodepoint(cp);
    unit.breakable_space = IsBreakableSpaceCodepoint(cp);
    unit.allow_break_after = false;
    unit.must_break_after = false;
    out->push_back(unit);

    char encoded[4];
    size_t encoded_len = EncodeUtf8(cp, encoded);
    if (!encoded_len)
      return false;
    normalized.append(encoded, encoded_len);

    prev_cp = cp;
    offset += step;
  }

  if (out->empty())
    return true;

  static std::vector<char> breaks;
  breaks.assign(out->size(), LINEBREAK_NOBREAK);
  size_t count = set_linebreaks_utf8_per_code_point(
      (const utf8_t *)normalized.data(), normalized.size(), lang, breaks.data());
  if (count != out->size())
    return false;

  for (size_t i = 0; i < out->size(); i++) {
    out->at(i).must_break_after = breaks[i] == LINEBREAK_MUSTBREAK;
    out->at(i).allow_break_after =
        breaks[i] == LINEBREAK_ALLOWBREAK || breaks[i] == LINEBREAK_MUSTBREAK;
  }
  return true;
}

size_t Utf8BytesForDisplayChars(const char *s, size_t display_chars) {
  if (!s || !*s || display_chars == 0)
    return 0;

  std::vector<TextCodepoint> run;
  if (!BuildTextRunUtf8(s, std::strlen(s), NULL, &run))
    return 0;

  size_t graphemes = 0;
  size_t bytes = 0;
  for (size_t i = 0; i < run.size(); i++) {
    if (run[i].grapheme_start) {
      if (graphemes == display_chars)
        break;
      graphemes++;
    }
    bytes = run[i].byte_offset + run[i].byte_length;
  }
  return bytes;
}

size_t StripLeadingListMarkerUtf8(const char *s) {
  return StripLeadingListMarkerUtf8(s, s ? std::strlen(s) : 0, NULL);
}

size_t StripLeadingListMarkerUtf8(const char *s, size_t len,
                                  bool *all_whitespace_only) {
  if (all_whitespace_only)
    *all_whitespace_only = false;
  if (!s || !len)
    return 0;

  size_t offset = 0;
  bool saw_non_whitespace = false;
  while (offset < len && s[offset] != '\0') {
    uint32_t cp = 0;
    size_t step = DecodeNextDisplayCodepoint(s + offset, len - offset, &cp);
    if (!step)
      break;

    if (IsUnicodeWhitespaceCodepoint(cp)) {
      offset += step;
      continue;
    }

    saw_non_whitespace = true;
    if (!IsLikelyListMarkerCodepoint(cp)) {
      size_t ordered_marker = ConsumeOrderedListMarkerUtf8(s + offset, len - offset);
      return ordered_marker ? offset + ordered_marker : 0;
    }

    offset += step;
    while (offset < len && s[offset] != '\0') {
      uint32_t next_cp = 0;
      size_t next_step =
          DecodeNextDisplayCodepoint(s + offset, len - offset, &next_cp);
      if (!next_step || !IsUnicodeWhitespaceCodepoint(next_cp))
        break;
      offset += next_step;
    }
    return offset;
  }

  if (!saw_non_whitespace && all_whitespace_only)
    *all_whitespace_only = true;
  return 0;
}

} // namespace text_unicode_utils
