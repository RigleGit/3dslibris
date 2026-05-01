#pragma once
#include "formats/epub/epub.h"
#include "book/book_parse_deps.h"
#include "expat.h"
#include "minizip/unzip.h"
#include <3ds.h>
#include <string>
#include <vector>
typedef BookParseDeps EpubDeps;

void epub_data_init(epub_data_t *d);
void epub_data_delete(epub_data_t *d);
void epub_container_start(void *data, const char *el, const char **attr);
void epub_rootfile_start(void *data, const char *el, const char **attr);
void epub_rootfile_end(void *data, const char *el);
void epub_rootfile_char(void *data, const XML_Char *txt, int len);
int epub_parse_currentfile(unzFile uf, epub_data_t *epd, const EpubDeps &deps,
                           unzFile css_scan_uf = NULL);
int LoadEpubPackageData(unzFile uf, Book *book, epub_data_t *parsedata,
                        std::string *opf_folder, const EpubDeps &deps);
int LoadEpubPackageForParse(unzFile uf, Book *book, epub_data_t *parsedata,
                            std::string *folder, bool metadataonly,
                            const EpubDeps &deps
#ifdef DSLIBRIS_DEBUG
                            , u64 *t_after_container, u64 *t_after_rootfile
#endif
);
void ApplyEpubMetadataOnlyResult(Book *book, epub_data_t &parsedata,
                                 const std::string &folder);
std::vector<std::string> BuildEpubSpineDocumentList(const epub_data_t &parsedata);
