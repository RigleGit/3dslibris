/*
    3dslibris - book_xml_block_handler.h

    Block element start/end handling.
    Extracted from book_xml_parser.cpp.

    Covers: html, body, aside, blockquote, caption, dd, div, dt, figure,
    h4–h6, head, ol, ul, li, p, hr, pre, br (end), script, style
    and the generic display:block promotion path.
    h1–h3 are handled by book_xml_heading_handler; a/img/table have their
    own handlers. title and FB2 branches remain in book_xml_parser.cpp.
*/

#pragma once

#include "book/epub_css_class_map.h"
#include "parse.h"

struct parsedata_t;
class Text;

namespace book_xml_block_handler {

// Handle block element start. Returns true if el was handled (book_xml_parser
// must not also push TAG_UNKNOWN). Returns false for unrecognised elements.
// If *out_early_return is set to true, the caller must return immediately
// after this call (used by the hidden-li early-exit path).
bool HandleBlockElementStart(parsedata_t *p, Text *ts, const char *el,
                              const char **attr,
                              const epub_css_class_map::CssClassMargins &elem_css,
                              const char *el_class_raw,
                              bool *out_early_return = nullptr);

// Handle block element end. Returns true if el was handled.
// Does NOT call parse_pop — that is always done by the caller after this returns.
bool HandleBlockElementEnd(parsedata_t *p, Text *ts, const char *el);

// Promote an inline element with CSS display:block to block layout.
// Emits linefeed, applies margins, queues top spacing.
// Call unconditionally — no-op if the element is a native block or lacks
// display:block, or if p->in_paragraph is true.
void ApplyDisplayBlockPromotion(parsedata_t *p, Text *ts, const char *el,
                                 const char **attr,
                                 const epub_css_class_map::CssClassMargins &elem_css);

} // namespace book_xml_block_handler
