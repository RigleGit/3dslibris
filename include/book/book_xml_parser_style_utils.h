#pragma once

#include "parse.h"
#include "shared/text_token_constants.h"

namespace book_xml_parser_style_utils {

inline u8 ResolveParsedTextStyle(bool bold, bool italic, bool mono) {
  if (mono && bold && italic)
    return TEXT_STYLE_MONO_BOLDITALIC;
  if (mono && bold)
    return TEXT_STYLE_MONO_BOLD;
  if (mono && italic)
    return TEXT_STYLE_MONO_ITALIC;
  if (mono)
    return TEXT_STYLE_MONO;
  if (bold && italic)
    return TEXT_STYLE_BOLDITALIC;
  if (bold)
    return TEXT_STYLE_BOLD;
  if (italic)
    return TEXT_STYLE_ITALIC;
  return TEXT_STYLE_REGULAR;
}

inline void RestoreParsedStyleMarkers(parsedata_t *p) {
  if (!p)
    return;
  if (p->superscript)
    parse_append_page_byte(p, TEXT_SUPERSCRIPT_ON);
  if (p->subscript)
    parse_append_page_byte(p, TEXT_SUBSCRIPT_ON);
  if (p->strikethrough)
    parse_append_page_byte(p, TEXT_STRIKETHROUGH_ON);
  if (p->underline)
    parse_append_page_byte(p, TEXT_UNDERLINE_ON);
  if (p->italic)
    parse_append_page_byte(p, TEXT_ITALIC_ON);
  if (p->bold)
    parse_append_page_byte(p, TEXT_BOLD_ON);
  if (p->mono)
    parse_append_page_byte(p, TEXT_MONO_ON);
}

} // namespace book_xml_parser_style_utils
