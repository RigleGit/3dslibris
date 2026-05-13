/*
    3dslibris - book_xml_inline_handler.h

    Inline element start/end handling and CSS-based inline styling.
    Extracted from book_xml_parser.cpp.

    Covers: named inline element start (strong/b, em/i, u/ins, strike, sup,
    sub, code, ruby/rp/rt), CSS-based inline styling for any element inside
    TAG_BODY, and inline style re-sync after parse_pop.
*/

#pragma once

#include "book/epub_css_class_map.h"
#include "parse.h"

struct parsedata_t;
class Text;

struct InlineHandlerFns {
  // Full chardata pipeline (needed for rt annotation parens).
  void (*emit_chardata)(parsedata_t *p, const char *txt, int len);
};

namespace book_xml_inline_handler {

// Handle named inline element start. Returns true if el was handled (caller
// must not also push TAG_UNKNOWN). Returns false if el is not a named inline
// element — caller should fall through to its default branch.
bool HandleNamedInlineElementStart(parsedata_t *p, Text *ts, const char *el,
                                    const char **attr,
                                    const InlineHandlerFns &fns);

// Apply CSS-based inline styling derived from attr/elem_css for any element
// that is a descendant of TAG_BODY. Safe to call unconditionally; no-op
// when parse_in(p, TAG_BODY) is false or stacksize == 0.
void HandleCssInlineStylingStart(parsedata_t *p, Text *ts, const char *el,
                                  const char **attr,
                                  const epub_css_class_map::CssClassMargins &elem_css);

// Re-derive and emit all inline style markers (bold/italic/underline/etc.)
// after a parse_pop. Call once per element end, after the pop. Also handles
// overflow check that may advance to the next screen.
void SyncInlineStyleAfterPop(parsedata_t *p, Text *ts);

} // namespace book_xml_inline_handler
