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
} epub_data_t;

int epub(Book *book, std::string filepath, bool metadataonly);
int epub_extract_cover(Book *book, const std::string &epubpath);
