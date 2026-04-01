/*
    3dslibris - epub_ncx_parser.h
    NCX and EPUB3 NAV document parsing.
    Extracted from epub.cpp by Rigle.
*/

#pragma once

#include <string>
#include <vector>

struct toc_entry_t {
  std::string href;
  std::string title;
  unsigned char level;
};

namespace epub_ncx_parser {

bool ParseNcxWithExpat(const std::string &xml, const std::string &base_path,
                       std::vector<toc_entry_t> *entries);

bool ParseNcxLightweight(const std::string &xml, const std::string &base_path,
                         std::vector<toc_entry_t> *entries);

bool ParseNavWithExpat(const std::string &xml, const std::string &base_path,
                       std::vector<toc_entry_t> *entries);

} // namespace epub_ncx_parser
