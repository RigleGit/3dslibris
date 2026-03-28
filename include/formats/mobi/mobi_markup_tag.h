#ifndef MOBI_MARKUP_TAG_H
#define MOBI_MARKUP_TAG_H

#include <stddef.h>
#include <string>

enum MobiMarkupTagKind {
  MOBI_MARKUP_TAG_OTHER = 0,
  MOBI_MARKUP_TAG_SCRIPT,
  MOBI_MARKUP_TAG_STYLE,
  MOBI_MARKUP_TAG_BR,
  MOBI_MARKUP_TAG_IMG,
  MOBI_MARKUP_TAG_LI,
  MOBI_MARKUP_TAG_BLOCK,
};

struct MobiMarkupTagInfo {
  bool valid;
  bool closing;
  MobiMarkupTagKind kind;
  int heading_level;
  size_t attrs_offset;
  size_t attrs_length;

  MobiMarkupTagInfo()
      : valid(false), closing(false), kind(MOBI_MARKUP_TAG_OTHER),
        heading_level(-1), attrs_offset(0), attrs_length(0) {}
};

bool mobi_parse_markup_tag(const std::string &tag_text, MobiMarkupTagInfo *out);
bool mobi_parse_markup_tag(const char *tag_text, size_t len,
                           MobiMarkupTagInfo *out);

#endif
