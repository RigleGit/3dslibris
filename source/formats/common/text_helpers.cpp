#include "formats/common/text_helpers.h"

#include "formats/common/rtf_control_word_utils.h"
#include "shared/utf8_utils.h"

#include <algorithm>
#include <cctype>
#include <vector>

bool LooksLikeValidUtf8Bytes(const std::string &s) {
  return utf8_utils::IsValidUtf8(s);
}

void AppendUtf8Codepoint(std::string *out, uint32_t cp) {
  if (!out)
    return;
  if (cp <= 0x7F) {
    out->push_back((char)cp);
  } else if (cp <= 0x7FF) {
    out->push_back((char)(0xC0 | (cp >> 6)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

void AppendCp1252Byte(std::string *out, unsigned char b) {
  static const uint16_t cp1252_map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  if (!out)
    return;
  if (b < 0x80) {
    out->push_back((char)b);
    return;
  }
  if (b >= 0x80 && b <= 0x9F) {
    uint16_t mapped = cp1252_map[b - 0x80];
    if (mapped != 0x0000)
      AppendUtf8Codepoint(out, mapped);
    else
      out->push_back('?');
    return;
  }
  AppendUtf8Codepoint(out, b);
}

std::string DecodeLegacySingleByteToUtf8(const std::string &in) {
  return utf8_utils::DecodeCp1252ToUtf8(in);
}

size_t CountUtf8InvalidLeadBytes(const std::string &bytes) {
  return utf8_utils::CountUtf8InvalidLeadBytes(bytes);
}

std::string DecodeMostlyUtf8WithCp1252Fallback(const std::string &in,
                                               size_t *invalid_out) {
  return utf8_utils::DecodeMostlyUtf8WithCp1252Fallback(in, invalid_out);
}

std::string NormalizeTextUtf8(std::string raw) {
  if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
      (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
    raw.erase(0, 3);
  }
  if (utf8_utils::IsValidUtf8(raw))
    return raw;
  return utf8_utils::DecodeCp1252ToUtf8(raw);
}

void NormalizeNewlines(std::string *s) {
  if (!s)
    return;
  if (s->find('\r') == std::string::npos)
    return;

  size_t write = 0;
  for (size_t read = 0; read < s->size(); read++) {
    char c = (*s)[read];
    if (c == '\r') {
      if (read + 1 < s->size() && (*s)[read + 1] == '\n')
        read++;
      (*s)[write++] = '\n';
    } else {
      (*s)[write++] = c;
    }
  }
  s->resize(write);
}

static int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string DecodeRtfToUtf8(const std::string &rtf) {
  std::string out;
  out.reserve(rtf.size());

  std::vector<uint8_t> skip_stack;
  skip_stack.push_back(0);

  for (size_t i = 0; i < rtf.size();) {
    bool skip = skip_stack.back();
    char c = rtf[i];

    if (c == '{') {
      skip_stack.push_back(skip);
      i++;
      continue;
    }
    if (c == '}') {
      if (skip_stack.size() > 1)
        skip_stack.pop_back();
      i++;
      continue;
    }
    if (c != '\\') {
      if (!skip)
        out.push_back(c);
      i++;
      continue;
    }

    if (i + 1 >= rtf.size()) {
      i++;
      continue;
    }

    char n = rtf[i + 1];
    if (n == '\\' || n == '{' || n == '}') {
      if (!skip)
        out.push_back(n);
      i += 2;
      continue;
    }
    if (n == '*') {
      skip_stack.back() = true;
      i += 2;
      continue;
    }
    if (n == '\'') {
      if (i + 3 < rtf.size()) {
        int h1 = HexDigit(rtf[i + 2]);
        int h2 = HexDigit(rtf[i + 3]);
        if (h1 >= 0 && h2 >= 0 && !skip) {
          unsigned char b = (unsigned char)((h1 << 4) | h2);
          AppendCp1252Byte(&out, b);
        }
        i += 4;
      } else {
        i += 2;
      }
      continue;
    }
    if (n == '~') {
      if (!skip)
        out.push_back(' ');
      i += 2;
      continue;
    }
    if (n == '_') {
      if (!skip)
        out.push_back('-');
      i += 2;
      continue;
    }
    if (n == '-') {
      i += 2;
      continue;
    }
    if (n == 'u') {
      size_t p = i + 2;
      int sign = 1;
      if (p < rtf.size() && (rtf[p] == '-' || rtf[p] == '+')) {
        if (rtf[p] == '-')
          sign = -1;
        p++;
      }
      int value = 0;
      bool any = false;
      while (p < rtf.size() && isdigit((unsigned char)rtf[p])) {
        value = value * 10 + (rtf[p] - '0');
        p++;
        any = true;
      }
      if (any && !skip) {
        int cp = sign * value;
        if (cp < 0)
          cp += 65536;
        if (cp >= 0)
          AppendUtf8Codepoint(&out, (uint32_t)cp);
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;
      if (p < rtf.size() && rtf[p] != '\\' && rtf[p] != '{' && rtf[p] != '}')
        p++;
      i = p;
      continue;
    }
    if (isalpha((unsigned char)n)) {
      size_t p = i + 1;
      while (p < rtf.size() && isalpha((unsigned char)rtf[p]))
        p++;
      const char *word = rtf.data() + i + 1;
      const size_t word_len = p - (i + 1);
      if (p < rtf.size() &&
          (rtf[p] == '-' || rtf[p] == '+' || isdigit((unsigned char)rtf[p]))) {
        p++;
        while (p < rtf.size() && isdigit((unsigned char)rtf[p]))
          p++;
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;

      if (!skip)
        rtf_control_word_utils::AppendReplacement(word, word_len, &out,
                                                  AppendUtf8Codepoint);
      i = p;
      continue;
    }

    i += 2;
  }

  return out;
}
