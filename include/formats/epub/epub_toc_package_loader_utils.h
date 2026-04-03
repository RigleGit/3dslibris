/*
    3dslibris - epub_toc_package_loader_utils.h

    EPUB package TOC loading utilities extracted from epub.cpp.
*/

#pragma once

#include <map>
#include <string>
#include <vector>

#include "formats/epub/epub.h"
#include "formats/epub/epub_ncx_parser.h"
#include "minizip/unzip.h"

class Book;
class IStatusReporter;

namespace epub_toc_package_loader_utils {

void BuildPageStartMapFromPackage(const epub_data_t &parsedata,
                                  const std::string &opf_folder, Book *book,
                                  std::map<std::string, u16> *out);

bool LoadTocEntriesFromPackage(unzFile uf, epub_data_t &parsedata,
                               const std::string &opf_folder,
                               std::vector<toc_entry_t> *toc_entries,
                               IStatusReporter *reporter);

} // namespace epub_toc_package_loader_utils
