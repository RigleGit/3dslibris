/*
    3dslibris - book_xml_heading_handler.h

    Heading element handler: h1–h3 start events with font-size application,
    block spacing, and keep-with-next logic. Also exports ApplyHeadingFontSize
    and RestoreHeadingFontSize (used by h4–h6 in book_xml_parser.cpp) and
    ShouldRenderHrRule (used by hr in start/end).
*/

#pragma once

#include "book/book_xml_css_style_utils.h"
#include "book/epub_css_class_map.h"

#include <string>

struct parsedata_t;
class Text;

// Callbacks into book_xml_parser.cpp for functions that live inside its
// anonymous namespace. Populated by the parser and passed to HandleHeadingStart.
struct HeadingHandlerFns {
  void (*ensure_block_boundary)(parsedata_t *p, const char *tag,
                                const char *phase);
  void (*advance_screen)(parsedata_t *p);
  void (*queue_block_spacing)(parsedata_t *p, const char *tag,
                              const char *phase,
                              const book_xml_css_style_utils::MarginTopResult &mtr,
                              int line_h, int default_lf);
};

// Apply or restore the heading-level font size. Also used for h4–h6.
void ApplyHeadingFontSize(parsedata_t *p, Text *ts, int heading_level,
                          const std::string &style_attr,
                          const std::string &class_attr);
void RestoreHeadingFontSize(parsedata_t *p, Text *ts);

// Returns false if the hr rule should be suppressed by CSS.
bool ShouldRenderHrRule(const std::string &style_attr,
                        const std::string &class_attr);

// Handle h1/h2/h3 start event. Returns after pushing the element onto the
// style stack.
void HandleHeadingStart(parsedata_t *p, Text *ts, const char **attr,
                        const epub_css_class_map::CssClassMargins &elem_css,
                        int heading_level, const HeadingHandlerFns &fns);
