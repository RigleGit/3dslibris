#pragma once

#include "formats/epub/epub.h"
#include "shared/status_reporter.h"
#include <map>
#include <3ds.h>

void ResolveEpubTocFromPackageData(
    unzFile uf, Book *book, epub_data_t &parsedata,
    const std::string &folder,
    const std::map<std::string, u16> &page_start_by_href,
    IStatusReporter *reporter);
// epub_resolve_toc is already declared in epub.h
