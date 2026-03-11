/*
    3dslibris - string_utils.h
    Shared string utilities extracted from duplicated static functions.
    Created by Rigle to reduce code duplication.
*/

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <algorithm>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <string>

// --- Trim leading/trailing whitespace ---

inline std::string Trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && isspace((unsigned char)s[start]))
    start++;
  size_t end = s.size();
  while (end > start && isspace((unsigned char)s[end - 1]))
    end--;
  return s.substr(start, end - start);
}

// --- ASCII lowercase ---

inline std::string ToLowerAscii(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

// --- Case-insensitive file extension check ---

inline bool HasExtCI(const char *name, const char *ext) {
  if (!name || !ext)
    return false;
  size_t nlen = strlen(name);
  size_t elen = strlen(ext);
  if (elen == 0 || nlen < elen)
    return false;
  return strcasecmp(name + nlen - elen, ext) == 0;
}

// --- FNV-1a 64-bit hash ---

inline uint64_t Fnv1a64(const std::string &s) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < s.size(); i++) {
    hash ^= (uint8_t)s[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

// --- HTML attribute parsing helpers ---

inline bool IsHtmlNameChar(unsigned char c) {
  return isalnum(c) || c == '_' || c == ':' || c == '-';
}

inline std::string ExtractHtmlAttrValue(const std::string &tag,
                                        const std::string &attr_name_lc) {
  size_t i = 0;
  while (i < tag.size()) {
    while (i < tag.size() && isspace((unsigned char)tag[i]))
      i++;
    if (i >= tag.size())
      break;

    size_t key0 = i;
    while (i < tag.size() && IsHtmlNameChar((unsigned char)tag[i]))
      i++;
    if (i == key0) {
      i++;
      continue;
    }
    std::string key = tag.substr(key0, i - key0);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)tolower(c); });

    while (i < tag.size() && isspace((unsigned char)tag[i]))
      i++;
    if (i >= tag.size() || tag[i] != '=')
      continue;
    i++;
    while (i < tag.size() && isspace((unsigned char)tag[i]))
      i++;
    if (i >= tag.size())
      break;

    char quote = 0;
    if (tag[i] == '"' || tag[i] == '\'') {
      quote = tag[i];
      i++;
    }
    size_t val0 = i;
    if (quote) {
      while (i < tag.size() && tag[i] != quote)
        i++;
      std::string val = tag.substr(val0, i - val0);
      if (i < tag.size())
        i++;
      if (key == attr_name_lc)
        return val;
    } else {
      while (i < tag.size() && !isspace((unsigned char)tag[i]) && tag[i] != '>')
        i++;
      if (key == attr_name_lc)
        return tag.substr(val0, i - val0);
    }
  }
  return "";
}

#endif // STRING_UTILS_H
