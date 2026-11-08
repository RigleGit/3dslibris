/*
    3dslibris - book_xml_table_handler.h

    Table capture handler: tracks cells/rows during XML parsing and emits
    formatted table lines into the page buffer when </table> is closed.
    Called from xml::book::start, xml::book::end, and xml::book::chardata.
*/

#pragma once

#include <string>

#include "book/epub_css_class_map.h"

struct parsedata_t;
class Text;

// Callbacks into book_xml_parser.cpp for functions that live inside its
// anonymous namespace. Populated by the parser and passed to each call.
struct TableHandlerFns {
  void (*linefeed)(parsedata_t *p);
  void (*flush_inline_tail)(parsedata_t *p, Text *ts);
  void (*emit_flowed_raw)(parsedata_t *p, const char *txt, int len);
};

// Called from xml::book::chardata to accumulate text into the active cell.
std::string *GetActiveCapturedTableText(parsedata_t *p);

// Returns true and handles the element if inside (or entering) a table.
bool HandleTableStart(parsedata_t *p, Text *ts, const char *el,
                      const char **attr,
                      const epub_css_class_map::CssClassMargins &elem_css,
                      const TableHandlerFns &fns);

// Returns true and handles the element if inside a table.
bool HandleTableEnd(parsedata_t *p, Text *ts, const char *el,
                    const TableHandlerFns &fns);
