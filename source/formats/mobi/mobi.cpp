/*
    3dslibris - mobi.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Inline-image path parsing/formatting for MOBI/AZW books.
    - Maps recindex attribute values from HTML tags to the
      "mobi:recindex:N" URL scheme used by the inline image cache.

    Cover extraction (EXTH hints, image-record scan, decode + scale)
    lives in mobi_cover_extract.cpp.
*/

#include "formats/mobi/mobi.h"

#include "shared/string_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>

static bool ParseMobiImageRefToken(const std::string &token, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;
  if (token.empty())
    return false;

  unsigned long parsed = 0;
  bool all_digits = true;
  for (size_t i = 0; i < token.size(); i++) {
    unsigned char c = (unsigned char)token[i];
    if (!isdigit(c)) {
      all_digits = false;
      break;
    }
    parsed = parsed * 10UL + (unsigned long)(c - '0');
    if (parsed > 65535UL)
      return false;
  }
  if (all_digits) {
    if (parsed == 0 || parsed > 65535UL)
      return false;
    if (recindex_out)
      *recindex_out = (u16)parsed;
    return true;
  }

  parsed = 0;
  for (size_t i = 0; i < token.size(); i++) {
    unsigned char c = (unsigned char)token[i];
    unsigned value = 0;
    if (c >= '0' && c <= '9') {
      value = (unsigned)(c - '0');
    } else if (c >= 'A' && c <= 'V') {
      value = 10u + (unsigned)(c - 'A');
    } else if (c >= 'a' && c <= 'v') {
      value = 10u + (unsigned)(c - 'a');
    } else {
      return false;
    }
    parsed = parsed * 32UL + (unsigned long)value;
    if (parsed > 65535UL)
      return false;
  }
  if (parsed == 0 || parsed > 65535UL)
    return false;
  if (recindex_out)
    *recindex_out = (u16)parsed;
  return true;
}

static bool ParseMobiImageRefValue(const std::string &value, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;
  const std::string trimmed = Trim(value);
  if (trimmed.empty())
    return false;

  if (ParseMobiImageRefToken(trimmed, recindex_out))
    return true;

  std::string lower = trimmed;
  for (size_t i = 0; i < lower.size(); i++)
    lower[i] = (char)tolower((unsigned char)lower[i]);
  static const char *kPrefixes[] = {"kindle:embed:", "kindle:flow:", "embed:"};
  for (size_t p = 0; p < sizeof(kPrefixes) / sizeof(kPrefixes[0]); p++) {
    const std::string prefix = kPrefixes[p];
    if (lower.compare(0, prefix.size(), prefix) != 0)
      continue;
    size_t start = prefix.size();
    size_t end = start;
    while (end < trimmed.size()) {
      unsigned char c = (unsigned char)trimmed[end];
      if (isalnum(c) == 0)
        break;
      end++;
    }
    if (end > start)
      return ParseMobiImageRefToken(trimmed.substr(start, end - start),
                                    recindex_out);
  }

  return false;
}

bool mobi_extract_image_recindex(const std::string &tag, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;

  static const char *kAttrs[] = {"recindex", "lowrecindex", "hirecindex",
                                 "src", "href"};
  for (size_t i = 0; i < sizeof(kAttrs) / sizeof(kAttrs[0]); i++) {
    std::string value = ExtractHtmlAttrValue(tag, kAttrs[i]);
    if (value.empty())
      continue;
    if (ParseMobiImageRefValue(value, recindex_out))
      return true;
  }
  return false;
}

std::string mobi_inline_image_path(u16 recindex) {
  char out[32];
  snprintf(out, sizeof(out), "mobi:recindex:%u", (unsigned)recindex);
  return std::string(out);
}

bool mobi_parse_inline_image_path(const std::string &path, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;

  static const char *kPrefix = "mobi:recindex:";
  if (path.compare(0, strlen(kPrefix), kPrefix) != 0)
    return false;

  const std::string value = path.substr(strlen(kPrefix));
  if (value.empty())
    return false;

  unsigned long parsed = 0;
  for (size_t i = 0; i < value.size(); i++) {
    unsigned char c = (unsigned char)value[i];
    if (!isdigit(c))
      return false;
    parsed = parsed * 10UL + (unsigned long)(c - '0');
    if (parsed == 0 || parsed > 65535UL)
      return false;
  }

  if (parsed == 0 || parsed > 65535UL)
    return false;
  if (recindex_out)
    *recindex_out = (u16)parsed;
  return true;
}
