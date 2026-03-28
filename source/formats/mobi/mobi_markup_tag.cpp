#include "formats/mobi/mobi_markup_tag.h"

#include <ctype.h>
#include <string.h>

namespace {

static char AsciiLower(char c) {
  if (c >= 'A' && c <= 'Z')
    return (char)(c - 'A' + 'a');
  return c;
}

static bool EqualsAsciiNoCase(const char *s, size_t start, size_t len,
                              const char *needle) {
  if (!needle)
    return false;
  const size_t needle_len = strlen(needle);
  if (len != needle_len)
    return false;
  for (size_t i = 0; i < len; i++) {
    if (AsciiLower(s[start + i]) != needle[i])
      return false;
  }
  return true;
}

static bool IsBlockTagName(const char *s, size_t start, size_t len) {
  static const char *kTags[] = {
      "p",      "div",   "section", "article",      "tr",    "table",
      "td",     "th",    "ul",      "ol",           "blockquote",
      "pre",    "header","footer",  "aside",        "title", "mbp:pagebreak",
  };
  for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++) {
    if (EqualsAsciiNoCase(s, start, len, kTags[i]))
      return true;
  }
  return false;
}

static int HeadingLevelForTagName(const char *s, size_t start, size_t len) {
  if (len == 2 && AsciiLower(s[start]) == 'h' && s[start + 1] >= '1' &&
      s[start + 1] <= '6') {
    return s[start + 1] - '1';
  }
  return -1;
}

} // namespace

bool mobi_parse_markup_tag(const char *tag_text, size_t len,
                           MobiMarkupTagInfo *out) {
  if (!out)
    return false;

  *out = MobiMarkupTagInfo();
  if (!tag_text)
    return false;

  size_t start = 0;
  size_t end = len;
  while (start < end && isspace((unsigned char)tag_text[start]))
    start++;
  while (end > start && isspace((unsigned char)tag_text[end - 1]))
    end--;
  if (start >= end)
    return false;

  if (tag_text[start] == '/') {
    out->closing = true;
    start++;
    while (start < end && isspace((unsigned char)tag_text[start]))
      start++;
  }
  if (start >= end)
    return false;

  size_t name_start = start;
  while (start < end) {
    const char c = tag_text[start];
    if (isspace((unsigned char)c) || c == '/')
      break;
    start++;
  }
  if (start == name_start)
    return false;

  const size_t name_len = start - name_start;
  out->valid = true;
  out->attrs_offset = start;
  out->attrs_length = end - start;

  out->heading_level = HeadingLevelForTagName(tag_text, name_start, name_len);
  if (EqualsAsciiNoCase(tag_text, name_start, name_len, "script")) {
    out->kind = MOBI_MARKUP_TAG_SCRIPT;
  } else if (EqualsAsciiNoCase(tag_text, name_start, name_len, "style")) {
    out->kind = MOBI_MARKUP_TAG_STYLE;
  } else if (EqualsAsciiNoCase(tag_text, name_start, name_len, "br")) {
    out->kind = MOBI_MARKUP_TAG_BR;
  } else if (EqualsAsciiNoCase(tag_text, name_start, name_len, "img")) {
    out->kind = MOBI_MARKUP_TAG_IMG;
  } else if (EqualsAsciiNoCase(tag_text, name_start, name_len, "li")) {
    out->kind = MOBI_MARKUP_TAG_LI;
  } else if (out->heading_level >= 0 ||
             IsBlockTagName(tag_text, name_start, name_len)) {
    out->kind = MOBI_MARKUP_TAG_BLOCK;
  }

  return true;
}

bool mobi_parse_markup_tag(const std::string &tag_text, MobiMarkupTagInfo *out) {
  return mobi_parse_markup_tag(tag_text.data(), tag_text.size(), out);
}
