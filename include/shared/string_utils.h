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

// --- Case-insensitive prefix check ---

inline bool StartsWithNoCase(const std::string &s, const char *prefix) {
  if (!prefix)
    return false;
  size_t len = strlen(prefix);
  if (s.size() < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)prefix[i];
    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
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

// --- Normalize ZIP entry names (backslash to forward slash) ---

inline std::string NormalizeZipEntryName(const std::string &name) {
  std::string n = name;
  std::replace(n.begin(), n.end(), '\\', '/');
  return n;
}

// --- ASCII-only case-insensitive string equality ---

inline bool EqualsAsciiNoCase(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
  }
  return true;
}

// --- Case-insensitive substring search ---

inline bool ContainsNoCase(const std::string &haystack,
                           const std::string &needle) {
  if (needle.empty())
    return true;
  if (haystack.empty())
    return false;
  std::string h = haystack;
  std::string n = needle;
  std::transform(h.begin(), h.end(), h.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  std::transform(n.begin(), n.end(), n.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return h.find(n) != std::string::npos;
}

// --- Sanitize a string for use as a FAT32 filename component ---

inline std::string SanitizeFat32Name(const std::string &input,
                                     size_t max_len = 80) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    unsigned char c = (unsigned char)input[i];
    if (c < 0x20 || c >= 0x80) {
      out.push_back('_');
      continue;
    }
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') {
      out.push_back('_');
      continue;
    }
    out.push_back((char)c);
  }
  // Collapse runs of underscores/spaces into a single underscore.
  std::string collapsed;
  bool last_sep = false;
  for (size_t i = 0; i < out.size(); i++) {
    bool sep = (out[i] == '_' || out[i] == ' ');
    if (sep) {
      if (!last_sep)
        collapsed.push_back('_');
      last_sep = true;
    } else {
      collapsed.push_back(out[i]);
      last_sep = false;
    }
  }
  // Trim leading/trailing underscores and dots.
  size_t s = 0, e = collapsed.size();
  while (s < e && (collapsed[s] == '_' || collapsed[s] == '.'))
    s++;
  while (e > s && (collapsed[e - 1] == '_' || collapsed[e - 1] == '.'))
    e--;
  std::string result = collapsed.substr(s, e - s);
  if (result.size() > max_len)
    result.resize(max_len);
  while (!result.empty() && (result.back() == '_' || result.back() == '.'))
    result.pop_back();
  if (result.empty())
    result = "book";
  return result;
}

// --- Token search in space-separated list ---

inline bool ContainsToken(const std::string &list, const std::string &token) {
  size_t start = 0;
  while (start < list.size()) {
    while (start < list.size() && isspace((unsigned char)list[start]))
      start++;
    size_t end = start;
    while (end < list.size() && !isspace((unsigned char)list[end]))
      end++;
    if (end > start && list.substr(start, end - start) == token)
      return true;
    start = end;
  }
  return false;
}

#endif // STRING_UTILS_H
