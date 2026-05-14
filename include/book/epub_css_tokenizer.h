/*
    3dslibris - epub_css_tokenizer.h
    New 3DS reader module by Rigle.

    Summary:
    - Raw CSS tokenization primitives: whitespace/comment skipping, character
      classification, and selector-list parsing.
    - Extracted from epub_css_class_map.cpp so the tokenizer layer can be
      used and tested independently of the class→property lookup layer.
*/

#pragma once

#include <stddef.h>
#include <string>
#include <vector>

namespace epub_css_tokenizer {

bool IsIdentChar(char c);
void SkipWhitespace(const char *s, size_t len, size_t *pos);
void SkipToChar(const char *s, size_t len, size_t *pos, char target);
void SkipBlockComment(const char *s, size_t len, size_t *pos);

// Splits a comma-separated CSS selector list, filling class_names_out with
// names from ".class" / "tag.class" selectors and element_names_out with bare
// element names ("p", "body", etc.). Either output vector may be null.
void ParseSelectorList(const std::string &selector_list,
                       std::vector<std::string> *class_names_out,
                       std::vector<std::string> *element_names_out);

} // namespace epub_css_tokenizer
