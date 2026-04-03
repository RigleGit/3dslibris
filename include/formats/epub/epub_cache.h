#pragma once
#include "book/book_parse_deps.h"
#include "formats/epub/epub.h"
#include "minizip/unzip.h"
#include <string>
typedef BookParseDeps EpubDeps;

int FinalizeEpubParse(unzFile uf, epub_data_t *parsedata, Book *book,
                      const std::string &name, const EpubDeps &deps,
                      int rc, bool save_cache);
