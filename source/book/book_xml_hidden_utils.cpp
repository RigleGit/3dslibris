#include "book/book_xml_hidden_utils.h"

#include <string.h>

namespace book_xml_hidden_utils {

namespace {

bool EqualsAsciiNoCaseLocal(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
  }
  return *a == 0 && *b == 0;
}

bool AttrNameEqualsLocal(const char *xml_name, const char *needle) {
  if (!xml_name || !needle)
    return false;
  const char *colon = strrchr(xml_name, ':');
  const char *local = colon ? colon + 1 : xml_name;
  return EqualsAsciiNoCaseLocal(local, needle);
}

bool AttrTruthyNoCaseLocal(const char *value) {
  if (!value || !value[0])
    return true;
  return EqualsAsciiNoCaseLocal(value, "1") ||
         EqualsAsciiNoCaseLocal(value, "true") ||
         EqualsAsciiNoCaseLocal(value, "yes") ||
         EqualsAsciiNoCaseLocal(value, "hidden");
}

bool HasTokenNoCase(const char *value, const char *token) {
  if (!value || !token || !token[0])
    return false;
  const size_t token_len = strlen(token);
  const char *p = value;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
      p++;
    const size_t len = (size_t)(p - start);
    if (len == token_len) {
      bool match = true;
      for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)start[i];
        unsigned char cb = (unsigned char)token[i];
        if (ca >= 'A' && ca <= 'Z')
          ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
          cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
          match = false;
          break;
        }
      }
      if (match)
        return true;
    }
  }
  return false;
}

} // namespace

bool IsCosmeticPageBreakElement(const char **attr) {
  if (!attr)
    return false;

  bool aria_hidden = false;
  bool semantic_pagebreak = false;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (AttrNameEqualsLocal(name, "aria-hidden")) {
      if (AttrTruthyNoCaseLocal(value))
        aria_hidden = true;
    } else if (AttrNameEqualsLocal(name, "type")) {
      if (HasTokenNoCase(value, "pagebreak"))
        semantic_pagebreak = true;
    } else if (AttrNameEqualsLocal(name, "role")) {
      if (HasTokenNoCase(value, "doc-pagebreak"))
        semantic_pagebreak = true;
    }
  }

  return aria_hidden && semantic_pagebreak;
}

} // namespace book_xml_hidden_utils
