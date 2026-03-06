#pragma once

#include "book.h"
#include <string>

typedef enum { PARSE_CONTAINER, PARSE_ROOTFILE, PARSE_CONTENT } epub_parse_t;

// <manifest> elements
typedef struct {
  std::string id;
  std::string href;
  std::string media_type;
  std::string properties;
} epub_item;

// <spine> elements
typedef struct {
  std::string idref;
} epub_itemref;

typedef struct {
  epub_parse_t type;
  std::vector<std::string *> ctx;
  std::string docpath;
  std::string rootfile;
  std::vector<epub_item *> manifest;
  std::vector<epub_itemref *> spine;
  Book *book;
  bool metadataonly;
  std::string title;
  std::string creator;
  std::string coverid; //! id of the cover image item
  std::string tocid;   //! id of the NCX item (EPUB2)
  std::string navid;   //! id of the nav document (EPUB3)
  std::string parsed_doc_title; //! per-XHTML parsed title/heading candidate
} epub_data_t;

int epub(Book *book, std::string filepath, bool metadataonly);
int epub_extract_cover(Book *book, const std::string &epubpath);
int epub_resolve_toc(Book *book, std::string filepath);
