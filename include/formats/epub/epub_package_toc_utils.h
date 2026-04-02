/*
    3dslibris - epub_package_toc_utils.h

    EPUB package and TOC proxy utility helpers extracted from epub.cpp.
*/

#pragma once

#include <map>
#include <set>
#include <string>

#include "formats/epub/epub.h"
#include "formats/epub/epub_zip_utils.h"
#include "minizip/unzip.h"

class IStatusReporter;

namespace epub_package_toc_utils {

std::string NormalizeFragmentId(const std::string &raw);
std::string ExtractHrefFragment(const std::string &href);

bool LocateZipEntrySafe(unzFile uf, const std::string &entry_path,
                        IStatusReporter *reporter, const char *tag,
                        epub_zip_utils::ZipEntryIndex *index = NULL);

bool ReadZipEntryText(unzFile uf, const std::string &path, std::string &out,
                      IStatusReporter *reporter = NULL, const char *tag = NULL,
                      epub_zip_utils::ZipEntryIndex *index = NULL);

bool LookupTocProxyHref(
    unzFile uf, const std::string &doc_path, const std::string &fragment_raw,
    std::map<std::string, std::map<std::string, std::string>> *cache,
    std::set<std::string> *attempted, std::string *href_out,
    IStatusReporter *reporter);

std::string BuildDocPath(const std::string &opf_folder, const std::string &href);

bool FindManifestItemPath(epub_data_t &data, const std::string &id,
                          const std::string &opf_folder, std::string &path_out);

} // namespace epub_package_toc_utils

