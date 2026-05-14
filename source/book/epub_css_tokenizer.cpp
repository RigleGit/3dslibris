/*
    3dslibris - epub_css_tokenizer.cpp
    New 3DS reader module by Rigle.

    Summary:
    - Raw CSS tokenization primitives extracted from epub_css_class_map.cpp.
    - Handles whitespace/comment skipping, ident-char classification, and
      selector-list parsing. No knowledge of CSS property values.
*/

#include "book/epub_css_tokenizer.h"

#include "shared/string_utils.h"

namespace epub_css_tokenizer {

namespace {

static bool ExtractSingleClassSelectorName(const std::string &selector,
                                           std::string *class_name_out) {
  if (!class_name_out)
    return false;
  const std::string trimmed = Trim(selector);
  if (trimmed.empty())
    return false;
  // Support ".class" and "tag.class". Reject combinators, descendant
  // selectors, pseudo classes, and compound selectors.
  size_t pos = 0;
  while (pos < trimmed.size() && IsIdentChar(trimmed[pos]))
    ++pos;
  if (pos >= trimmed.size() || trimmed[pos] != '.')
    return false;
  ++pos;
  const size_t class_start = pos;
  while (pos < trimmed.size() && IsIdentChar(trimmed[pos]))
    ++pos;
  if (pos == class_start || pos != trimmed.size())
    return false;
  *class_name_out = trimmed.substr(class_start, pos - class_start);
  return true;
}

static bool ExtractBareElementSelectorName(const std::string &selector,
                                           std::string *el_out) {
  if (!el_out)
    return false;
  const std::string trimmed = Trim(selector);
  if (trimmed.empty())
    return false;
  if (!(trimmed[0] >= 'a' && trimmed[0] <= 'z') &&
      !(trimmed[0] >= 'A' && trimmed[0] <= 'Z'))
    return false;
  for (size_t i = 0; i < trimmed.size(); i++) {
    if (!IsIdentChar(trimmed[i]))
      return false;
  }
  *el_out = trimmed;
  return true;
}

} // namespace

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

void SkipWhitespace(const char *s, size_t len, size_t *pos) {
  while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' ||
                        s[*pos] == '\r' || s[*pos] == '\n'))
    ++(*pos);
}

void SkipToChar(const char *s, size_t len, size_t *pos, char target) {
  while (*pos < len && s[*pos] != target)
    ++(*pos);
}

void SkipBlockComment(const char *s, size_t len, size_t *pos) {
  if (*pos + 1 < len && s[*pos] == '/' && s[*pos + 1] == '*') {
    *pos += 2;
    while (*pos + 1 < len) {
      if (s[*pos] == '*' && s[*pos + 1] == '/') {
        *pos += 2;
        return;
      }
      ++(*pos);
    }
    *pos = len;
  }
}

void ParseSelectorList(const std::string &selector_list,
                       std::vector<std::string> *class_names_out,
                       std::vector<std::string> *element_names_out) {
  size_t start = 0;
  while (start <= selector_list.size()) {
    size_t comma = selector_list.find(',', start);
    std::string selector =
        selector_list.substr(start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start);
    std::string name;
    if (class_names_out && ExtractSingleClassSelectorName(selector, &name))
      class_names_out->push_back(name);
    else if (element_names_out &&
             ExtractBareElementSelectorName(selector, &name))
      element_names_out->push_back(name);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
}

} // namespace epub_css_tokenizer
